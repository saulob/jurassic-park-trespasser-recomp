#!/usr/bin/env python3
"""src/host/minimap_map.inc against the retail levels (game/data), headless:
py -3 -I -m unittest discover -s tests -p "test_minimap_map.py"
"""
import math
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import minimap_bounds  # noqa: E402
import minimap_map  # noqa: E402
from minimap_map import BUILDING, GATE, LAND, ROAD, WALL, WATER  # noqa: E402

DATA = ROOT / "game" / "data"
INC = ROOT / "src" / "host" / "minimap_map.inc"
MAX_INC_BYTES = 1_500_000


def quad(cx, cy, length, width, angle=0.0):
    ux, uy = math.cos(angle), math.sin(angle)
    hl, hw = length / 2, width / 2
    return [(cx + ux * u - uy * v, cy + uy * u + ux * v) for u, v in ((-hl, -hw), (hl, -hw), (hl, hw), (-hl, hw))]


class WallMerge(unittest.TestCase):
    def test_collinear_pieces_become_one_line(self):
        pieces = [quad(x, 0, 8, 2) for x in (0, 8.5, 17, 25.5)]
        merged = minimap_map.merge_walls(pieces)
        self.assertEqual(len(merged), 1)
        self.assertAlmostEqual(max(minimap_map.rect_sides(merged[0])), 33.5, delta=0.6)

    def test_a_real_gap_stays_open(self):
        merged = minimap_map.merge_walls([quad(0, 0, 8, 2), quad(8 + 4 + 4, 0, 8, 2)])
        self.assertEqual(len(merged), 2)

    def test_pieces_that_turn_a_corner_are_not_joined(self):
        merged = minimap_map.merge_walls([quad(0, 0, 10, 2), quad(6, 6, 10, 2, math.pi / 2)])
        self.assertEqual(len(merged), 2)

    def test_distant_parallel_lines_stay_apart(self):
        self.assertEqual(len(minimap_map.merge_walls([quad(0, 0, 20, 2), quad(0, 9, 20, 2)])), 2)

    def test_touching_parallel_pieces_are_one_wall(self):
        self.assertEqual(len(minimap_map.merge_walls([quad(0, 0, 20, 2), quad(0, 2.5, 20, 2)])), 1)

    def test_a_diagonal_line_merges_too(self):
        angle = math.radians(30)
        pieces = [quad(i * 8.3 * math.cos(angle), i * 8.3 * math.sin(angle), 8, 2, angle) for i in range(5)]
        self.assertEqual(len(minimap_map.merge_walls(pieces)), 1)

    def test_a_short_piece_bridges_two_long_ones(self):
        merged = minimap_map.merge_walls([quad(0, 0, 10, 2), quad(14.5, 0, 10, 2), quad(7.2, 0, 3, 2)])
        self.assertEqual(len(merged), 1)

    def test_short_wall_lines_are_not_thinner_than_the_minimum(self):
        a, b = minimap_map.rect_sides(minimap_map.line_quad((0, 0), (1.5, 0)))
        self.assertGreaterEqual(min(a, b), minimap_map.LANDMARK_MIN_WIDTH_M - 1e-9)

    def test_merging_is_order_independent(self):
        pieces = [quad(x, 0, 8, 2) for x in (0, 8.5, 17)] + [quad(40, 5, 6, 2, 1.0)]
        self.assertEqual(sorted(minimap_map.merge_walls(pieces)), sorted(minimap_map.merge_walls(pieces[::-1])))


class Classification(unittest.TestCase):
    def test_landmark_kinds(self):
        def obj(name, cls="CInstance"):
            return {"name": name, "class": cls}

        kind = minimap_map.landmark_kind
        self.assertEqual(kind(obj("FTownWall03-")), BUILDING)
        self.assertEqual(kind(obj("FHelipad-")), BUILDING)
        self.assertEqual(kind(obj("SElecFence", None)), BUILDING)
        self.assertEqual(kind(obj("SHrbrGate", None)), GATE)
        self.assertEqual(kind(obj("SHammondGateWall", None)), BUILDING)
        self.assertEqual(kind(obj("TrnObj_Street01", "CTerrainObj")), ROAD)
        self.assertEqual(kind(obj("TrnObj_SmmtCon03", "CTerrainObj")), BUILDING)
        for helper in ("FInvisWall01-", "FMonoPhysicsBBox02-", "FWaterfallBox", "NoBakersville03-"):
            self.assertIsNone(kind(obj(helper)), helper)
        for natural in ("VTree01", "ARaptor", "$FPcrate", "FCanyonRock03", "SBeach_Island01"):
            self.assertIsNone(kind(obj(natural, None if natural[0] == "S" else "CInstance")), natural)
        for not_built in ("Baker", "AIDrew", "Occlusion", "$JRaptorA", "PtruckBody"):
            self.assertIsNone(kind(obj(not_built, None)), not_built)
        for terrain in ("TrnObj_Mntn", "TrnObj_MtnSide", "TrnObj_GrssSnd", "TrnObj_Base", "TrnObj_Veg"):
            self.assertIsNone(kind(obj(terrain, "CTerrainObj")), terrain)

    def test_water_sources(self):
        def obj(name, cls, **props):
            return {"name": name, "class": cls, "props": props}

        self.assertTrue(minimap_map.is_water_surface(obj("TBeachPond", "CEntityWater")))
        self.assertTrue(minimap_map.is_water_surface(obj("Ocean", "CInstance", Anim00="Ocean01t2.bmp")))
        self.assertFalse(minimap_map.is_water_surface(obj("WF", "CInstance", Anim00="TIJWF05_0000t2.BMP")))
        self.assertFalse(minimap_map.is_water_surface(obj("Pscreen", "CInstance", Anim00="Pcraydisplay0000000.bmp")))
        self.assertTrue(minimap_map.is_water_terrain(obj("TrnObj_PhilLagoonBtm01", "CTerrainObj", SoundMaterial="Terrain - Water")))
        for dry in ("TrnObj_StreamBed9", "TrnObj_IJStream12"):
            self.assertFalse(minimap_map.is_water_terrain(obj(dry, "CTerrainObj", SoundMaterial="Terrain - Dirt")), dry)
        self.assertFalse(minimap_map.is_water_terrain(obj("TrnObj_OceanSound00", "CTerrainObj", SoundMaterial="Terrain - Water2")))
        self.assertTrue(minimap_map.is_sea_tile(obj("TrnObj_Ocean3", "CTerrainObj")))
        self.assertFalse(minimap_map.is_land_object(obj("TrnObj_Ocean3", "CTerrainObj")))


class Vehicles(unittest.TestCase):
    BODIES = ("PHJeepCrshed00-00", "PHJeepCrshSuvy00-910", "PHunterJeepCrashed00-02", "PHunterJeepSuvy00-00",
              "PHunterJeep00-00", "PHjeepOpenBody00-01", "PHjeepOpenBodyHack00-00", "PHjeepSLDseatBody00-00",
              "PtruckBody00-04", "PMercedesUnimoBody00-01")
    NOT_BODIES = ("PtruckDoorL00-00", "PtruckHood00-00", "PtruckLoorR-00", "PHunterJeepDoorL-00", "PHunterJeepDoorR00-00",
                  "PHjeepOpenSeat01-00", "PHjeepOpenStWeel00-00", "PHjeepOpenwindow00-00", "PMercedesUnimoBdoor00-00",
                  "FMercTruck27-00", "FPTruck14-00", "$FPHjeepBody01-00", "PAplaneCrate00", "PHelicopter06-00",
                  "Phelicopter00-00", "SMonoRailTrack4-00", "TRocB_16InchDia-01", "PCrate00-00", "Pdoor00-00", "Pbox-00")

    def test_a_vehicle_body_is_recognised(self):
        for name in self.BODIES:
            self.assertTrue(minimap_map.VEHICLE_BODY.match(name), name)

    def test_parts_and_common_objects_are_not_vehicles(self):
        for name in self.NOT_BODIES:
            self.assertFalse(minimap_map.VEHICLE_BODY.match(name), name)

    def test_the_bodies_of_one_vehicle_give_one_marker(self):
        near = [(10.0, 10.0, 1.0, 0.0), (11.5, 10.5, 0.0, 1.0), (12.0, 9.0, 1.0, 0.0)]
        self.assertEqual(len(minimap_map.merge_vehicles(near)), 1)

    def test_separate_vehicles_stay_separate(self):
        apart = [(0.0, 0.0, 1.0, 0.0), (4.2, 0.0, 1.0, 0.0), (100.0, 5.0, 0.0, 1.0)]
        self.assertEqual(len(minimap_map.merge_vehicles(apart)), 3)

    def test_merging_keeps_the_first_marker(self):
        kept = minimap_map.merge_vehicles([(5.0, 5.0, 1.0, 0.0), (6.0, 5.0, 0.0, 1.0)])
        self.assertEqual(kept, [(5.0, 5.0, 1.0, 0.0)])


@unittest.skipUnless((DATA / "be.scn").exists(), "retail level data not installed")
class MinimapMap(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.names = minimap_bounds.available_levels(DATA)
        cls.levels = [minimap_map.build_level(DATA, name) for name in cls.names]
        cls.by_id = {lv["id"]: lv for lv in cls.levels}

    def kinds(self, lv):
        out = [0, 0, 0, 0]
        for kind, _ in lv["landmarks"]:
            out[kind] += 1
        return out

    def water_cells(self, lv):
        cells = set()
        for start, length, row, kind in lv["runs"]:
            if kind == WATER:
                cells.update((c, row) for c in range(start, start + length))
        return cells

    def cell(self, lv, x, y):
        b = minimap_bounds.bounds_for(DATA, lv["id"].lower())
        return (int((x - b["min_x"]) / ((b["max_x"] - b["min_x"]) / lv["cols"])),
                int((b["max_y"] - y) / ((b["max_y"] - b["min_y"]) / lv["rows"])))

    def test_every_level_has_geometry(self):
        for lv in self.levels:
            self.assertTrue(any(kind == LAND for _, _, _, kind in lv["runs"]), lv["id"])

    def test_extra_maps_are_present(self):
        for name in ("TESTSCENE", "TESTSCNNGHT", "DEMO"):
            self.assertIn(name, self.by_id)
            self.assertGreater(len(self.by_id[name]["runs"]), 0, name)

    def test_water_is_derived(self):
        for name in ("BE", "JR", "IJ", "LAB", "AS"):
            self.assertGreater(len(self.water_cells(self.by_id[name])), 0, name)
        for name in ("SUM", "AS2"):
            self.assertEqual(len(self.water_cells(self.by_id[name])), 0, name)

    def test_water_sources_are_surfaces_not_beds(self):
        for lv in self.levels:
            for name, cls, extent, level, role in lv["water_sources"]:
                self.assertNotRegex(name, r"(?i)stream|lakebed|sound", lv["id"])
                if role == "plane":
                    self.assertIn(cls, ("CInstance",), name)
                    self.assertGreater(extent, minimap_map.PLANE_M, name)
                if cls == "CTerrainObj":
                    self.assertLessEqual(extent, minimap_map.PATCH_MAX_M, name)
        be = [s for s in self.by_id["BE"]["water_sources"] if s[4] == "plane"]
        self.assertEqual([s[0] for s in be], ["Ocean-00"])
        self.assertAlmostEqual(be[0][3], 12.1, places=1)

    def test_every_water_entity_is_in_the_water_mask(self):
        for name in ("be", "jr", "ij", "lab"):
            lv = self.by_id[name.upper()]
            cells = self.water_cells(lv)
            res = minimap_map.analyse_level(DATA, name)
            entities = [o for o in res["objects"] if o["class"] == "CEntityWater"]
            self.assertTrue(entities, name)
            for o in entities:
                c, r = self.cell(lv, o["position"][0], o["position"][1])
                near = {(c + dc, r + dr) for dc in (-1, 0, 1) for dr in (-1, 0, 1)}
                self.assertTrue(near & cells, "%s %s" % (name, o["name"]))

    def test_dry_ground_is_not_water(self):
        be, jr = self.by_id["BE"], self.by_id["JR"]
        self.assertNotIn(self.cell(be, -1883.9, -831.6), self.water_cells(be))
        self.assertNotIn(self.cell(jr, -2053.8, 450.6), self.water_cells(jr))
        self.assertIn(self.cell(be, -1833.4, -986.3), self.water_cells(be))

    def test_runs_inside_mask(self):
        for lv in self.levels:
            self.assertGreater(lv["cols"], 0)
            self.assertLessEqual(lv["cols"], minimap_map.MAX_COLS)
            for start, length, row, kind in lv["runs"]:
                self.assertLess(row, lv["rows"], lv["id"])
                self.assertGreater(length, 0)
                self.assertLess(start, lv["cols"], lv["id"])
                self.assertLessEqual(start + length, lv["cols"], lv["id"])
                self.assertIn(kind, (LAND, WATER))

    def test_size_limits(self):
        self.assertLessEqual(INC.stat().st_size, MAX_INC_BYTES)
        for lv in self.levels:
            self.assertLessEqual(len(lv["runs"]), 3500, lv["id"])
            self.assertLessEqual(len(lv["landmarks"]), 900, lv["id"])

    def test_landmarks_are_real_footprints_inside_the_level(self):
        for lv in self.levels:
            b = minimap_bounds.bounds_for(DATA, lv["id"].lower())
            for kind, q in lv["landmarks"]:
                self.assertEqual(len(q), 4)
                for x, y in q:
                    self.assertTrue(math.isfinite(x) and math.isfinite(y), lv["id"])
                    self.assertTrue(b["min_x"] - 250 <= x <= b["max_x"] + 250 and b["min_y"] - 250 <= y <= b["max_y"] + 250,
                                    "%s landmark outside the level" % lv["id"])
                a, c = minimap_map.rect_sides(q)
                self.assertGreaterEqual(min(a, c), minimap_map.LANDMARK_MIN_WIDTH_M - 0.25, lv["id"])
                if kind == WALL:
                    self.assertLessEqual(min(a, c), minimap_map.WALL_MAX_WIDTH_M + 0.5, lv["id"])
                if kind == GATE:
                    self.assertLessEqual(max(a, c), minimap_map.GATE_MAX_M + 0.5, lv["id"])

    def test_no_giant_false_squares(self):
        for lv in self.levels:
            for kind, q in lv["landmarks"]:
                a, c = minimap_map.rect_sides(q)
                if kind in (BUILDING, GATE):
                    self.assertLessEqual(min(a, c), 45.0, "%s: a %s %.0f x %.0f m block" % (lv["id"], kind, a, c))
                    self.assertLessEqual(a * c, 4200.0, "%s: a %.0f x %.0f m block" % (lv["id"], a, c))

    def test_walls_are_merged_into_lines(self):
        sum_walls = [max(minimap_map.rect_sides(q)) for k, q in self.by_id["SUM"]["landmarks"] if k == WALL]
        self.assertGreaterEqual(max(sum_walls), 40.0)
        it_walls = [max(minimap_map.rect_sides(q)) for k, q in self.by_id["IT"]["landmarks"] if k == WALL]
        self.assertGreaterEqual(sum(it_walls) / len(it_walls), 9.0)
        self.assertGreaterEqual(max(it_walls), 100.0)

    def test_buildings_and_gates_are_kept(self):
        it = self.kinds(self.by_id["IT"])
        self.assertGreaterEqual(it[BUILDING], 100)
        self.assertGreaterEqual(it[WALL], 150)
        self.assertGreaterEqual(it[ROAD], 100)
        self.assertGreaterEqual(it[GATE], 5)
        self.assertGreaterEqual(self.kinds(self.by_id["LAB"])[GATE], 10)
        self.assertGreaterEqual(sum(self.kinds(self.by_id["LAB"])[:2]), 150)

    def test_summit_has_structures(self):
        sum_kinds = self.kinds(self.by_id["SUM"])
        self.assertGreaterEqual(sum_kinds[BUILDING], 15)
        self.assertGreaterEqual(sum_kinds[WALL], 40)
        self.assertGreaterEqual(len(self.by_id["SUM"]["landmarks"]), 60)

    def test_deterministic(self):
        for name in ("be", "sum", "testscene"):
            again = minimap_map.build_level(DATA, name)
            first = self.by_id[name.upper()]
            self.assertEqual(first["runs"], again["runs"], name)
            self.assertEqual(first["landmarks"], again["landmarks"], name)

    def test_vehicles_are_the_retail_ones(self):
        counts = {lv["id"]: len(lv["vehicles"]) for lv in self.levels}
        self.assertGreaterEqual(counts["IJ"], 10)
        self.assertGreaterEqual(counts["BE"], 3)
        self.assertGreaterEqual(counts["AS"], 5)
        self.assertGreaterEqual(counts["JR"], 3)
        self.assertEqual(counts["SUM"], 0)
        for lv in self.levels:
            b = minimap_bounds.bounds_for(DATA, lv["id"].lower())
            for x, y, ax, ay in lv["vehicles"]:
                self.assertTrue(b["min_x"] <= x <= b["max_x"] and b["min_y"] <= y <= b["max_y"], (lv["id"], x, y))
                self.assertAlmostEqual(math.hypot(ax, ay), 1.0, delta=0.01)
            for i, a in enumerate(lv["vehicles"]):
                for other in lv["vehicles"][i + 1:]:
                    self.assertGreaterEqual(math.hypot(a[0] - other[0], a[1] - other[1]), minimap_map.VEHICLE_MERGE_M)

    def test_a_vehicle_is_not_a_landmark(self):
        for lv in self.levels:
            for x, y, _ax, _ay in lv["vehicles"]:
                for kind, q in lv["landmarks"]:
                    if kind in (BUILDING, GATE):
                        cx, cy = sum(p[0] for p in q) / 4, sum(p[1] for p in q) / 4
                        self.assertGreater(math.hypot(x - cx, y - cy), 1.0, (lv["id"], x, y))

    def test_committed_file_matches_a_fresh_generation(self):
        original = minimap_map.OUT
        with tempfile.TemporaryDirectory() as folder:
            temp = Path(folder) / "minimap_map.inc"
            minimap_map.OUT = temp
            try:
                minimap_map.emit(self.levels)
                self.assertEqual(temp.read_bytes(), INC.read_bytes())
            finally:
                minimap_map.OUT = original


if __name__ == "__main__":
    unittest.main()
