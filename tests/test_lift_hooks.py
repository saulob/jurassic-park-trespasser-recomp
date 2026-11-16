#!/usr/bin/env python3
"""tools/lift_hooks.py, headless: py -3 -m unittest discover -s tests -p "test_lift_hooks.py" """
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import lift_hooks  # noqa: E402


class Insn:
    def __init__(self, address):
        self.address = address


class Lifter:
    def __init__(self):
        self._flag_state = "cmp"

    def lift_instruction(self, insn):
        return [f"/* 0x{insn.address:08X}: nop */"]


class LiftHooks(unittest.TestCase):
    def test_project_list(self):
        vas = lift_hooks.listed_hooks()
        self.assertEqual(vas, sorted(set(vas)))
        self.assertTrue(all(0x00401000 <= va < 0x0063B000 for va in vas))

    def test_list_must_be_sorted_and_unique(self):
        with tempfile.TemporaryDirectory() as d:
            for text in ("LIFT_HOOK(00402000)\nLIFT_HOOK(00401000)\n", "LIFT_HOOK(00401000)\nLIFT_HOOK(00401000)\n"):
                path = Path(d) / "list.h"
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(SystemExit):
                    lift_hooks.listed_hooks(path)
            path.write_text("", encoding="utf-8")
            self.assertEqual(lift_hooks.listed_hooks(path), [])

    def test_hook_goes_in_front_of_its_instruction_only(self):
        lifter = Lifter()
        lift_hooks.install(lifter, [0x00401005])
        self.assertEqual(lifter.lift_instruction(Insn(0x00401000)), ["/* 0x00401000: nop */"])
        self.assertEqual(lifter._flag_state, "cmp")
        self.assertEqual(lifter.lift_instruction(Insn(0x00401005)),
                         ["RECOMP_LIFT_HOOK(lift_hook_00401005); /* host hook */", "/* 0x00401005: nop */"])
        self.assertIsNone(lifter._flag_state)

    def test_header_declares_each_hook(self):
        text = lift_hooks.header_text([0x00401005, 0x00402000])
        self.assertIn("#define RECOMP_LIFT_HOOK(fn)", text)
        self.assertIn("void lift_hook_00401005(void);\n", text)
        self.assertIn("void lift_hook_00402000(void);\n", text)

    def test_counts_every_copy(self):
        with tempfile.TemporaryDirectory() as d:
            call = lift_hooks.hook_call(0x00401005)
            Path(d, "recomp_0000.c").write_text(f"{call}\n{call}\n", encoding="utf-8")
            Path(d, "recomp_0001.c").write_text("\n", encoding="utf-8")
            self.assertEqual(lift_hooks.hook_counts(d, [0x00401005, 0x00402000]), {0x00401005: 2, 0x00402000: 0})


if __name__ == "__main__":
    unittest.main()
