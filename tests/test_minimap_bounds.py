#!/usr/bin/env python3
"""src/host/minimap_bounds.inc against the retail levels (game/data), headless:
py -3 -I -m unittest discover -s tests -p "test_minimap_bounds.py"
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import minimap_bounds  # noqa: E402

DATA = ROOT / "game" / "data"
INC = ROOT / "src" / "host" / "minimap_bounds.inc"
ENTRY = re.compile(r'\{\s*"([A-Z0-9]+)",\s*(-?[0-9]+)\.0f,\s*(-?[0-9]+)\.0f,\s*(-?[0-9]+)\.0f,\s*(-?[0-9]+)\.0f\s*\}')
NUM_TOKEN = re.compile(r'-?[0-9]+(?:\.[0-9]+)?f')
C_FLOAT = re.compile(r'^-?[0-9]+\.[0-9]+f$')  # a decimal point is required before the f suffix
BARE_SUFFIX = re.compile(r'^-?[0-9]+f$')       # e.g. -3329f: MSVC C rejects this (C2059)


def read_inc():
    return [(m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))
            for m in ENTRY.finditer(INC.read_text(encoding="ascii"))]


class MinimapBoundsLiterals(unittest.TestCase):
    def test_every_numeric_literal_is_valid_c(self):
        tokens = NUM_TOKEN.findall(INC.read_text(encoding="ascii"))
        self.assertTrue(tokens, "the file has float literals")
        for token in tokens:
            self.assertRegex(token, C_FLOAT, "invalid C float literal %r: needs a decimal point before f" % token)
            self.assertNotRegex(token, BARE_SUFFIX, "bare integer with f suffix %r breaks MSVC C" % token)


@unittest.skipUnless((DATA / "be.scn").exists(), "retail level data not installed")
class MinimapBounds(unittest.TestCase):
    def test_inc_matches_the_levels(self):
        expected = [minimap_bounds.bounds_for(DATA, level) for level in minimap_bounds.available_levels(DATA)]
        got = read_inc()
        self.assertEqual(len(got), len(expected), "one row per level")
        for e, g in zip(expected, got):
            self.assertEqual(g, (e["id"], e["min_x"], e["min_y"], e["max_x"], e["max_y"]))

    def test_level_ids(self):
        self.assertEqual([e[0] for e in read_inc()], [l.upper() for l in minimap_bounds.available_levels(DATA)])


if __name__ == "__main__":
    unittest.main()
