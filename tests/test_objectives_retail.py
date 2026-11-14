#!/usr/bin/env python3
"""src/host/objective_policy.c against the retail levels (game/data), headless:
py -3 -I -m unittest discover -s tests -p "test_objectives_retail.py"
"""
import math
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import scn_triggers  # noqa: E402

DATA = ROOT / "game" / "data"
TABLE = ROOT / "src" / "host" / "objective_policy.c"
LEVELS = {"be": "BE", "jr": "JR", "ij": "IJ", "it": "IT", "lab": "LAB", "as": "AS", "as2": "AS2", "sum": "SUM"}
END_GAME = 31
ASCENT = {"AS", "AS2", "SUM"}

ENTRY = re.compile(r'\{\s*(\d+),\s*"([^"]+)",\s*\{([^}]*)\},\s*(NULL|"[^"]*"),\s*\{([^}]*)\}\s*\}', re.S)
ARRAY = re.compile(r'static const ob_objective_t k_(\w+)\[\] = \{(.*?)\n\};', re.S)


def names(text):
    return [n for n in re.findall(r'"([^"]+)"', text)]


def table():
    out = {}
    for level, body in ARRAY.findall(TABLE.read_text(encoding="utf-8")):
        out[level.upper()] = [
            {"hint": int(h), "title": t, "target": names(tg), "carried": None if c == "NULL" else c.strip('"'),
             "route": names(r)}
            for h, t, tg, c, r in ENTRY.findall(body)
        ]
    return out


def flat(p, q):
    return math.hypot(p[0] - q[0], p[1] - q[1])


@unittest.skipUnless((DATA / "be.scn").exists(), "retail level data not installed")
class RetailObjectives(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.table = table()
        cls.levels = {LEVELS[k]: scn_triggers.analyse_level(DATA, k) for k in LEVELS}

    def objects(self, level):
        return {o["name"]: o for o in self.levels[level]["objects"]}

    def hint_records(self, level):
        out = {}
        for h in self.levels[level]["hints"]:
            out.setdefault(h["hint_id"], []).append(h)
        return out

    def test_every_campaign_level_has_objectives(self):
        self.assertEqual(sorted(self.table), sorted(LEVELS.values()))

    def test_hints_are_the_levels_own_in_order(self):
        for level, items in self.table.items():
            hints = [i["hint"] for i in items]
            self.assertEqual(hints, sorted(set(self.hint_records(level))), level)

    def test_hint_families(self):
        for level, items in self.table.items():
            family = {"BE": 1, "JR": 2, "IJ": 3, "IT": 4, "LAB": 6, "AS": 7, "AS2": 8, "SUM": 9}[level]
            self.assertTrue(all(i["hint"] // 100 == family for i in items), level)

    def linked(self, level, target, later_hints):
        """target is the trigger of a later hint, one of its collision elements, or upstream of it."""
        records = self.hint_records(level)
        for r in (r for h in later_hints for r in records[h]):
            trig = self.levels[level]["triggers"][r["trigger"]]
            if target == r["trigger"] or target in (trig.get("elements") or []):
                return True
            if any(u["name"] == target for u in r.get("upstream") or []):
                return True
            for u in r.get("upstream") or []:
                up = self.levels[level]["triggers"].get(u["name"])
                if up and target in (up.get("elements") or []):
                    return True
        return False

    def finishes(self, level, target):
        trig = self.levels[level]["triggers"].get(target)
        if not trig:
            return False
        exits = {e["trigger"] for e in self.levels[level]["level_exits"]}
        return target in exits or any(a["action_type"] == END_GAME for a in trig["actions"])

    def test_targets_lead_to_the_next_hint_or_out(self):
        for level, items in self.table.items():
            objects = self.objects(level)
            for n, item in enumerate(items):
                later = [i["hint"] for i in items[n + 1:]]
                self.assertTrue(item["target"], "%s %d has no target" % (level, item["hint"]))
                for target in item["target"]:
                    self.assertIn(target, objects, "%s %d: %s is not in the level" % (level, item["hint"], target))
                    ok = self.linked(level, target, later[:2]) if later else self.finishes(level, target)
                    self.assertTrue(ok, "%s %d: %s does not lead on" % (level, item["hint"], target))
                if item["carried"]:
                    self.assertIn(item["carried"], objects)
                    self.assertTrue(self.linked(level, item["carried"], later), item["carried"])

    def test_routes_are_teleports_closing_in(self):
        for level, items in self.table.items():
            objects = self.objects(level)
            for item in items:
                if not item["route"]:
                    continue
                anchors = [objects[n] for n in item["route"]]
                self.assertTrue(all(a["class"] in ("Teleport", None) for a in anchors), item["route"])
                target = objects[item["target"][0]]["position"]
                if level in ASCENT:
                    heights = [a["position"][2] for a in anchors] + [target[2]]
                    ok = all(b > a for a, b in zip(heights, heights[1:])) or \
                        all(flat(a["position"], target) > flat(b["position"], target) for a, b in zip(anchors, anchors[1:]))
                else:
                    gaps = [flat(a["position"], target) for a in anchors]
                    ok = all(a > b for a, b in zip(gaps, gaps[1:]))
                self.assertTrue(ok, "%s %d: the route does not close in on its target" % (level, item["hint"]))

    def test_titles_are_short(self):
        for level, items in self.table.items():
            for item in items:
                self.assertTrue(item["title"] and len(item["title"]) < 48, item["title"])

    def test_every_hint_trigger_is_a_known_kind(self):
        for level in self.table:
            for records in self.hint_records(level).values():
                for r in records:
                    self.assertIn(r["class"], ("CStartTrigger", "CLocationTrigger", "CBooleanTrigger", "CCollisionTrigger"))


if __name__ == "__main__":
    unittest.main()
