#!/usr/bin/env python3
"""tools/stable_entries.py, headless: py -3 -m unittest discover -s tests -p "test_stable_entries.py" """
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import stable_entries  # noqa: E402

BODY = """void sub_00401000(void) {
    RECOMP_ENTER(0x00401000u);
    RECOMP_CALL(sub_00401000); /* recursion stays on the stable entry */
    RECOMP_CALL(sub_00402000);
}
"""


class StableEntries(unittest.TestCase):
    def test_project_list(self):
        vas = stable_entries.listed_overrides()
        self.assertEqual(vas, sorted(set(vas)))
        self.assertTrue(all(0x00401000 <= va < 0x0063B000 for va in vas))

    def test_list_must_be_sorted_and_unique(self):
        with tempfile.TemporaryDirectory() as d:
            for text in ("GUEST_OVERRIDE(00402000, REQUIRED)\nGUEST_OVERRIDE(00401000, OPTIONAL)\n",
                         "GUEST_OVERRIDE(00401000, REQUIRED)\nGUEST_OVERRIDE(00401000, OPTIONAL)\n", ""):
                path = Path(d) / "list.h"
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(SystemExit):
                    stable_entries.listed_overrides(path)

    def test_rename_keeps_calls(self):
        renamed = stable_entries.rename_definition(BODY, 0x00401000)
        self.assertTrue(renamed.startswith("void lifted_sub_00401000(void) {"))
        self.assertIn("RECOMP_CALL(sub_00401000);", renamed)
        self.assertIn("RECOMP_CALL(sub_00402000);", renamed)
        alias = "void sub_00401010(void) { g_alias_entry = 0x00401010u; sub_00401000(); }\n"
        self.assertEqual(stable_entries.rename_definition(alias, 0x00401010),
                         "void lifted_sub_00401010(void) { g_alias_entry = 0x00401010u; sub_00401000(); }\n")

    def test_rename_needs_one_definition(self):
        with self.assertRaises(SystemExit):
            stable_entries.rename_definition(BODY, 0x00402000)
        with self.assertRaises(SystemExit):
            stable_entries.rename_definition(BODY + BODY, 0x00401000)

    def test_check_folder(self):
        vas = stable_entries.listed_overrides()
        with tempfile.TemporaryDirectory() as d:
            text = "".join(f"void {stable_entries.lifted_name(va)}(void) {{\n}}\n" for va in vas)
            Path(d, "recomp_0000.c").write_text(text, encoding="utf-8")
            self.assertEqual(stable_entries.check_folder(d)[1], [])
            Path(d, "recomp_0001.c").write_text(f"void sub_{vas[0]:08X}(void) {{\n}}\n", encoding="utf-8")
            self.assertEqual(len(stable_entries.check_folder(d)[1]), 1)


if __name__ == "__main__":
    unittest.main()
