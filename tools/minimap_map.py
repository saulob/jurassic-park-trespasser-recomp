#!/usr/bin/env python3
"""Regenerate the minimap geometry in src/host/minimap_map.inc from the retail levels (game/data).

Everything is derived from the GRF only (no SPZ/TIN) and drawn by the host over the level's world bounds
(tools/minimap_bounds.py). Two raster layers, smoothed at run time into soft contours:

  * land: the playable area, from the XY positions of the level's objects and the footprints of their terrain pieces.
    Objects standing below a water surface do not make land.
  * water: the real water surfaces (CEntityWater meshes, the animated Ocean planes) and the terrain the game gives a
    water sound material. A large plane only counts where nothing stands above its level.

and the vector landmarks, derived from the real mesh triangles of constructed objects, in world metres:
buildings (solid footprints; hollow ones such as courtyard walls become their wall lines), walls and fences (collinear
pieces merged into continuous lines), roads and gates. Props, crates, collision volumes, invisible walls and natural
features are not landmarks. Ground vehicles (jeeps, trucks, the Unimog) are one marker each: the centre and long axis
of the body's footprint.

    py -3 tools/minimap_map.py [--data game/data] [--stats] [--svg <dir>] [--report <dir>]
"""
import argparse
import math
import re
import struct
import sys
from collections import deque
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from scn_triggers import Groff, analyse_level, rotation_matrix
import minimap_bounds

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "src" / "host" / "minimap_map.inc"
MASK_H = 256          # rows; columns follow the bounds aspect ratio
MAX_COLS = 640
LAND_STAMP_M = 26.0   # radius of the land stamp around one object
LAND_MIN_CELLS = 150  # connected land blobs smaller than this are dropped
DRY_CORE_M = 14.0     # an object above a water plane keeps that much of the plane dry around it
PLANE_M = 150.0       # a water surface longer than this is a plane that land can cover
PATCH_MAX_M = 150.0   # water terrain longer than this is a base, not a pool
LAND_PATCH_M = 70.0   # a terrain piece longer than this is a base, not a patch of land
GATE_MAX_M = 25.0     # a longer "gate" is a gatehouse or a wall
SUBMERGED_M = 0.5     # below the level of a water plane by more than this is under water
LANDMARK_MIN_M = 8.0  # minimum length of a building, wall or road piece
LINEAR_MIN_M = 2.0     # pieces of a wall, fence, walkway or bridge are kept from this length and merged into lines
GATE_MIN_M = 3.0
LANDMARK_MAX_M = 300.0  # longer than this is a backdrop or a bounding object, not a structure
LANDMARK_MIN_WIDTH_M = 2.0  # thin walls and fences are drawn at least this thick
WALL_ASPECT = 4.0     # a footprint this much longer than wide is a wall or fence
WALL_MAX_WIDTH_M = 6.0
SOLID_FILL = 0.45     # a mesh whose triangles cover less of its hull than this is hollow
HOLLOW_MIN_M = 15.0
VERTICAL_NZ = 0.3     # a triangle this upright is a wall face
SEGMENT_MIN_M = 1.5
MERGE_ANGLE = 0.14    # sine of the angle within which wall pieces are collinear
MERGE_GAP_M = 2.5     # a wall opening at least this wide is kept
MERGE_SIDE_M = 1.0    # parallel pieces closer than this (edge to edge) are one wall
LAND, WATER = 1, 2
BUILDING, WALL, ROAD, GATE = 0, 1, 2, 3
KIND_NAMES = ("building", "wall", "road", "gate")

# First letter of a name that is not a constructed object (vegetation, terrain, AI, actors, boulders/raptors, occlusion,
# child parts, water, backdrop).
SKIP_LETTERS = set("VTABROo$W")

# Collision volumes, invisible walls and no-go markers: not something the player sees.
HELPER_NAME = re.compile(r"(?i:invis|physics|bbox|box)|^F?No[A-Z]")
NATURAL_NAME = re.compile(r"rock|boulder|cliff|canyon|island|ridge|mound|hill|mntn|mtn|swamp", re.IGNORECASE)
ROAD_TERRAIN = re.compile(r"road|street|path", re.IGNORECASE)
BUILT_TERRAIN = re.compile(r"smmtcon|walkway|prkng|plza|porch|slab|pndwall|wallbase|compflr|hackflr|loshack|biolb|"
                           r"secgate|bnkr|flr", re.IGNORECASE)
GATE_NAME = re.compile(r"gate", re.IGNORECASE)
LINEAR_NAME = re.compile(r"wall|fence|barrier|wwy|walkway|bridge|pier|dock|ramp", re.IGNORECASE)
SEA_NAME = re.compile(r"ocean", re.IGNORECASE)
# The body of a ground vehicle: jeeps, trucks and the Mercedes Unimog. Doors, wheels, seats and windows are separate
# objects with other names, so one vehicle is one body.
VEHICLE_BODY = re.compile(r"^P(HJeepCrsh(ed|Suvy)|HunterJeep(Crashed|Suvy)?|HjeepOpenBody(Hack)?|HjeepSLDseatBody|truckBody|"
                          r"MercedesUnimoBody)\d*-\d+$")
VEHICLE_MERGE_M = 3.0  # bodies closer than this are one vehicle
VEHICLE_SMALL, VEHICLE_MEDIUM, VEHICLE_LARGE = 0, 1, 2
VEHICLE_LARGE_NAME = re.compile(r"truckBody|MercedesUnimoBody")
VEHICLE_MEDIUM_NAME = re.compile(r"CrshSuvy|JeepSuvy|JeepCrshed")


def vehicle_size(name):
    """Trucks and the Unimog are large, the closed jeeps and SUVs medium, the open jeeps small."""
    if VEHICLE_LARGE_NAME.search(name):
        return VEHICLE_LARGE
    return VEHICLE_MEDIUM if VEHICLE_MEDIUM_NAME.search(name) else VEHICLE_SMALL
WATER_SOUND = re.compile(r"water", re.IGNORECASE)


def props_of(obj):
    return obj.get("props") or {}


def is_water_surface(obj):
    if obj["class"] == "CEntityWater":
        return True
    if obj["class"] != "CInstance":
        return False
    return any("ocean" in str(v).lower() for k, v in props_of(obj).items() if str(k).startswith("Anim"))


def is_water_terrain(obj):
    name = obj["name"] or ""
    return obj["class"] == "CTerrainObj" and "sound" not in name.lower() and \
        bool(WATER_SOUND.search(str(props_of(obj).get("SoundMaterial", ""))))


def is_sea_tile(obj):
    name = obj["name"] or ""
    return obj["class"] in ("CTerrainObj", "CInstance") and bool(SEA_NAME.search(name)) and "sound" not in name.lower()


def is_land_object(obj):
    return minimap_bounds.is_land(obj) and not is_sea_tile(obj) and not is_water_surface(obj) and \
        not is_water_terrain(obj)


def is_terrain_patch(obj):
    return obj["class"] == "CTerrainObj" and not is_sea_tile(obj) and not is_water_terrain(obj) and \
        minimap_bounds.is_land(obj)


def landmark_kind(obj):
    """BUILDING, ROAD or GATE for a constructed object (walls are buildings that turn out long and thin), else None."""
    name = obj["name"] or ""
    cls = obj["class"]
    if not name or HELPER_NAME.search(name):
        return None
    if cls == "CTerrainObj":
        if "sound" in name.lower():
            return None
        if ROAD_TERRAIN.search(name):
            return ROAD
        return BUILDING if BUILT_TERRAIN.search(name) else None
    if cls is None:
        if name[0] not in "SF":
            return None
    elif cls != "CInstance" or name[0] in SKIP_LETTERS or name.startswith("BackDrop"):
        return None
    if NATURAL_NAME.search(name):
        return None
    return GATE if GATE_NAME.search(name) and not LINEAR_NAME.search(name) else BUILDING


def geometry_points(groff, handle):
    sec = groff.by_handle.get(handle)
    if not sec or sec["size"] < 28:
        return None
    _name, geom = struct.unpack_from("<2I", groff.section_bytes(sec), 0)
    gsec = groff.by_handle.get(geom)
    if not gsec:
        return None
    g = groff.section_bytes(gsec)
    if len(g) >= 40:
        _px, _py, _pz, nv, nf, nvn, nw = struct.unpack_from("<3f4I", g, 0)
        if 40 + 12 * (nv + 2 * nf + nvn + nw) == len(g) and nv:
            return [struct.unpack_from("<3f", g, 40 + 12 * i) for i in range(nv)]
    if len(g) >= 44:
        _px, _py, _pz = struct.unpack_from("<3f", g, 12)
        npoints, _nv, _np, nwrap, _npolys = struct.unpack_from("<5I", g, 24)
        if 44 + 12 * (npoints + nwrap) <= len(g) and npoints:
            pts = [struct.unpack_from("<3f", g, 44 + 12 * i) for i in range(npoints)]
            if nwrap:
                base = len(g) - 12 * nwrap
                pts = [struct.unpack_from("<3f", g, base + 12 * i) for i in range(nwrap)]
            return pts
    return None


def geometry_mesh(groff, handle):
    """(points, triangles) of the mesh form that stores one 32-byte record per triangle corner, else None."""
    sec = groff.by_handle.get(handle)
    if not sec or sec["size"] < 28:
        return None
    _name, geom = struct.unpack_from("<2I", groff.section_bytes(sec), 0)
    gsec = groff.by_handle.get(geom)
    if not gsec:
        return None
    g = groff.section_bytes(gsec)
    if len(g) < 44:
        return None
    _px, _py, _pz, nv0, nf, nvn, nw = struct.unpack_from("<3f4I", g, 0)
    if 40 + 12 * (nv0 + 2 * nf + nvn + nw) == len(g):
        return None
    npoints, corners, _np, nwrap, _npolys = struct.unpack_from("<5I", g, 24)
    records = 44 + 12 * npoints
    if not npoints or not corners or corners % 3 or records + 32 * corners > len(g) or 44 + 12 * (npoints + nwrap) > len(g):
        return None
    index = [struct.unpack_from("<I", g, records + 32 * i)[0] for i in range(corners)]
    if max(index) >= npoints:
        return None
    pts = [struct.unpack_from("<3f", g, 44 + 12 * i) for i in range(npoints)]
    return pts, [(index[i], index[i + 1], index[i + 2]) for i in range(0, corners, 3)]


def world_points(obj, pts):
    m = rotation_matrix(*obj["rotation"])
    scale = obj["scale"]
    out = []
    for p in pts:
        v = (p[0] * scale, p[1] * scale, p[2] * scale)
        out.append((obj["position"][0] + m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
                    obj["position"][1] + m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
                    obj["position"][2] + m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]))
    return out


def convex_hull(points):
    pts = sorted(set(points))
    if len(pts) <= 2:
        return pts

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower = []
    for p in pts:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    upper = []
    for p in reversed(pts):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def hull_of(world):
    hull = convex_hull([(p[0], p[1]) for p in world])
    if len(hull) < 3:
        return None, 0.0
    w = max(p[0] for p in hull) - min(p[0] for p in hull)
    h = max(p[1] for p in hull) - min(p[1] for p in hull)
    return hull, max(w, h)


def polygon_area(poly):
    return abs(sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1]
                   for i in range(len(poly)))) / 2


def oriented_rect(hull):
    best = None
    for i in range(len(hull)):
        x0, y0 = hull[i]
        x1, y1 = hull[(i + 1) % len(hull)]
        length = math.hypot(x1 - x0, y1 - y0)
        if length == 0.0:
            continue
        ux, uy = (x1 - x0) / length, (y1 - y0) / length
        us = [(x - x0) * ux + (y - y0) * uy for x, y in hull]
        vs = [-(x - x0) * uy + (y - y0) * ux for x, y in hull]
        area = (max(us) - min(us)) * (max(vs) - min(vs))
        if best is None or area < best[0] - 1e-9:
            best = (area, x0, y0, ux, uy, min(us), max(us), min(vs), max(vs))
    _, x0, y0, ux, uy, u0, u1, v0, v1 = best
    return [(x0 + ux * u - uy * v, y0 + uy * u + ux * v) for u, v in ((u0, v0), (u1, v0), (u1, v1), (u0, v1))]


def widen_rect(quad):
    """Every side at least LANDMARK_MIN_WIDTH_M, grown about the centre."""
    cx = sum(p[0] for p in quad) / 4
    cy = sum(p[1] for p in quad) / 4
    ux, uy = quad[1][0] - quad[0][0], quad[1][1] - quad[0][1]
    vx, vy = quad[3][0] - quad[0][0], quad[3][1] - quad[0][1]
    lu, lv = math.hypot(ux, uy), math.hypot(vx, vy)
    if lu >= LANDMARK_MIN_WIDTH_M and lv >= LANDMARK_MIN_WIDTH_M:
        return quad
    wu, wv = max(lu, LANDMARK_MIN_WIDTH_M) / 2, max(lv, LANDMARK_MIN_WIDTH_M) / 2
    ux, uy = ux / max(lu, 1e-9) * wu, uy / max(lu, 1e-9) * wu
    vx, vy = vx / max(lv, 1e-9) * wv, vy / max(lv, 1e-9) * wv
    return [(cx - ux - vx, cy - uy - vy), (cx + ux - vx, cy + uy - vy), (cx + ux + vx, cy + uy + vy),
            (cx - ux + vx, cy - uy + vy)]


def rect_sides(quad):
    return math.dist(quad[0], quad[1]), math.dist(quad[0], quad[3])


def triangle_contains(px, py, a, b, c):
    d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    if abs(d) < 1e-12:
        return False
    l1 = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) / d
    l2 = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) / d
    return l1 >= -1e-9 and l2 >= -1e-9 and 1 - l1 - l2 >= -1e-9


def covered_fraction(tris, hull):
    """The share of the hull's area the projected triangles cover (vertical faces cover none)."""
    area = polygon_area(hull)
    if area <= 0:
        return 0.0
    xs = [p[0] for p in hull]
    ys = [p[1] for p in hull]
    step = max(0.8, math.sqrt(area / 5000))
    x0, y0 = min(xs), min(ys)
    covered = set()
    for a, b, c in tris:
        lx, hx = min(a[0], b[0], c[0]), max(a[0], b[0], c[0])
        ly, hy = min(a[1], b[1], c[1]), max(a[1], b[1], c[1])
        for i in range(int((lx - x0) / step), int((hx - x0) / step) + 2):
            for j in range(int((ly - y0) / step), int((hy - y0) / step) + 2):
                if (i, j) not in covered and triangle_contains(x0 + (i + 0.5) * step, y0 + (j + 0.5) * step, a, b, c):
                    covered.add((i, j))
    return len(covered) * step * step / area


def wall_lines(world, tris):
    """The XY lines of a mesh's upright faces, rounded and unique."""
    lines = {}
    for a, b, c in tris:
        pa, pb, pc = world[a], world[b], world[c]
        ux, uy, uz = pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]
        vx, vy, vz = pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        length = math.sqrt(nx * nx + ny * ny + nz * nz)
        if length < 1e-9 or abs(nz) / length > VERTICAL_NZ:
            continue
        p, q = max(((pa, pb), (pb, pc), (pa, pc)), key=lambda e: math.hypot(e[0][0] - e[1][0], e[0][1] - e[1][1]))
        if math.hypot(p[0] - q[0], p[1] - q[1]) < SEGMENT_MIN_M:
            continue
        key = tuple(sorted(((round(p[0], 1), round(p[1], 1)), (round(q[0], 1), round(q[1], 1)))))
        lines[key] = True
    return sorted(lines)


def line_quad(p, q):
    length = math.hypot(q[0] - p[0], q[1] - p[1])
    ux, uy = (q[0] - p[0]) / length, (q[1] - p[1]) / length
    pad = max(0.0, (LANDMARK_MIN_WIDTH_M - length) / 2)
    p, q = (p[0] - ux * pad, p[1] - uy * pad), (q[0] + ux * pad, q[1] + uy * pad)
    h = LANDMARK_MIN_WIDTH_M / 2
    return [(p[0] - uy * h, p[1] + ux * h), (q[0] - uy * h, q[1] + ux * h), (q[0] + uy * h, q[1] - ux * h),
            (p[0] + uy * h, p[1] - ux * h)]


def piece_frame(quad):
    """(centre x, centre y, unit axis x, unit axis y, half length, half width) of a rectangle."""
    a, b = rect_sides(quad)
    e = (quad[1][0] - quad[0][0], quad[1][1] - quad[0][1]) if a >= b else (quad[3][0] - quad[0][0], quad[3][1] - quad[0][1])
    length = math.hypot(*e) or 1.0
    return (sum(p[0] for p in quad) / 4, sum(p[1] for p in quad) / 4, e[0] / length, e[1] / length,
            max(a, b) / 2, min(a, b) / 2)


class WallGroup:
    """A wall line grown from its longest piece: later pieces join while they continue it or run beside it."""

    def __init__(self, quad):
        self.cx, self.cy, self.ux, self.uy, hl, hw = piece_frame(quad)
        self.u0, self.u1, self.v0, self.v1 = -hl, hl, -hw, hw

    def extent_of(self, quad):
        cx, cy, ex, ey, hl, hw = piece_frame(quad)
        sine = abs(self.ux * ey - self.uy * ex)
        cosine = math.sqrt(max(0.0, 1 - sine * sine))
        du = (cx - self.cx) * self.ux + (cy - self.cy) * self.uy
        dv = -(cx - self.cx) * self.uy + (cy - self.cy) * self.ux
        along, across = hl * cosine + hw * sine, hw * cosine + hl * sine
        return sine, (du - along, du + along, dv - across, dv + across)

    def accepts(self, quad):
        sine, (u0, u1, v0, v1) = self.extent_of(quad)
        if sine > MERGE_ANGLE or max(self.u0 - u1, u0 - self.u1) > MERGE_GAP_M or max(self.v0 - v1, v0 - self.v1) > MERGE_SIDE_M:
            return None
        return u0, u1, v0, v1

    def take(self, extent):
        u0, u1, v0, v1 = extent
        self.u0, self.u1 = min(self.u0, u0), max(self.u1, u1)
        self.v0, self.v1 = min(self.v0, v0), max(self.v1, v1)

    def quad(self):
        def at(u, v):
            return (self.cx + self.ux * u - self.uy * v, self.cy + self.uy * u + self.ux * v)

        return [at(self.u0, self.v0), at(self.u1, self.v0), at(self.u1, self.v1), at(self.u0, self.v1)]


def merge_walls(quads):
    """Collinear and touching wall pieces become one continuous line; openings of MERGE_GAP_M or more stay open."""
    for _ in range(6):
        ordered = sorted(quads, key=lambda q: (-round(max(rect_sides(q)), 3), round(sum(p[0] for p in q), 3),
                                               round(sum(p[1] for p in q), 3)))
        groups = []
        for quad in ordered:
            for group in groups:
                extent = group.accepts(quad)
                if extent:
                    group.take(extent)
                    break
            else:
                groups.append(WallGroup(quad))
        if len(groups) == len(quads):
            break
        quads = [g.quad() for g in groups]
    return [g.quad() for g in groups]


class Grid:
    def __init__(self, cols, rows, bounds):
        self.cols, self.rows = cols, rows
        self.min_x, self.max_y = bounds["min_x"], bounds["max_y"]
        self.cw = (bounds["max_x"] - bounds["min_x"]) / cols
        self.ch = (bounds["max_y"] - bounds["min_y"]) / rows
        count = cols * rows
        self.land = bytearray(count)
        self.dry = bytearray(count)
        self.plane = bytearray(count)
        self.plane_z = [0.0] * count

    def cell_of(self, x, y):
        return int((x - self.min_x) / self.cw), int((self.max_y - y) / self.ch)

    def inside(self, c, r):
        return 0 <= c < self.cols and 0 <= r < self.rows

    def centre(self, c, r):
        return self.min_x + (c + 0.5) * self.cw, self.max_y - (r + 0.5) * self.ch

    def stamp_disk(self, mask, x, y, radius_m):
        cx, cy = self.cell_of(x, y)
        rc, rr = int(math.ceil(radius_m / self.cw)) + 1, int(math.ceil(radius_m / self.ch)) + 1
        for r in range(max(0, cy - rr), min(self.rows - 1, cy + rr) + 1):
            for c in range(max(0, cx - rc), min(self.cols - 1, cx + rc) + 1):
                px, py = self.centre(c, r)
                if (px - x) ** 2 + (py - y) ** 2 <= radius_m * radius_m:
                    mask[r * self.cols + c] = 1

    def cover_triangles(self, mask, tris):
        """Marks the cells at least 4 of 9 sample points of which triangles cover; a sliver marks its own cell."""
        counts = {}
        for a, b, c in tris:
            c0, r1 = self.cell_of(min(a[0], b[0], c[0]), min(a[1], b[1], c[1]))
            c1, r0 = self.cell_of(max(a[0], b[0], c[0]), max(a[1], b[1], c[1]))
            hit = False
            for r in range(max(0, r0), min(self.rows - 1, r1) + 1):
                for col in range(max(0, c0), min(self.cols - 1, c1) + 1):
                    px, py = self.centre(col, r)
                    n = 0
                    for dx in (-1, 0, 1):
                        for dy in (-1, 0, 1):
                            n += triangle_contains(px + dx * self.cw / 3, py + dy * self.ch / 3, a, b, c)
                    if n:
                        counts[(col, r)] = counts.get((col, r), 0) + n
                        hit = True
            if not hit:
                col, r = self.cell_of((a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3)
                if self.inside(col, r):
                    counts[(col, r)] = 9
        cells = [cell for cell, n in counts.items() if n >= 4]
        if not cells and counts:
            cells = [max(sorted(counts), key=counts.get)]
        for col, r in cells:
            mask[r * self.cols + col] = 1
        return cells

    def cover_hull(self, mask, hull):
        xs = [p[0] for p in hull]
        ys = [p[1] for p in hull]
        c0, r1 = self.cell_of(min(xs), min(ys))
        c1, r0 = self.cell_of(max(xs), max(ys))
        c0, c1 = max(0, c0), min(self.cols - 1, c1)
        r0, r1 = max(0, r0), min(self.rows - 1, r1)
        edges = []
        for i in range(len(hull)):
            (x0, y0), (x1, y1) = hull[i], hull[(i + 1) % len(hull)]
            nx, ny = y1 - y0, x0 - x1
            edges.append((nx, ny, nx * x0 + ny * y0))
        hw, hh = self.cw / 2, self.ch / 2
        cells = []
        for r in range(r0, r1 + 1):
            for c in range(c0, c1 + 1):
                px, py = self.centre(c, r)
                if all(nx * px + ny * py - (abs(nx) * hw + abs(ny) * hh) <= k + 1e-9 for nx, ny, k in edges):
                    mask[r * self.cols + c] = 1
                    cells.append((c, r))
        return cells


def drop_small_blobs(cells, cols, rows, minimum):
    seen = bytearray(cols * rows)
    for y0 in range(rows):
        for x0 in range(cols):
            if not cells[y0 * cols + x0] or seen[y0 * cols + x0]:
                continue
            queue, blob = deque([(x0, y0)]), []
            seen[y0 * cols + x0] = 1
            while queue:
                x, y = queue.popleft()
                blob.append((x, y))
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < cols and 0 <= ny < rows and not seen[ny * cols + nx] and cells[ny * cols + nx]:
                        seen[ny * cols + nx] = 1
                        queue.append((nx, ny))
            if len(blob) < minimum:
                for x, y in blob:
                    cells[y * cols + x] = 0


def to_runs(cells, cols, rows, kind):
    runs = []
    for y in range(rows):
        x = 0
        while x < cols:
            if not cells[y * cols + x]:
                x += 1
                continue
            start = x
            while x < cols and cells[y * cols + x]:
                x += 1
            runs.append((start, x - start, y, kind))
    return runs


def water_surfaces(groff, objects):
    """Every water surface and water terrain of a level in world XY: triangles when the mesh has them, else its hull."""
    items = []
    for o in objects:
        if not (is_water_surface(o) or is_water_terrain(o)):
            continue
        handle = int(o["object_section"], 16)
        mesh = geometry_mesh(groff, handle)
        pts = mesh[0] if mesh else geometry_points(groff, handle)
        if not pts:
            continue
        world = world_points(o, pts)
        hull, extent = hull_of(world)
        if not hull or (is_water_terrain(o) and extent > PATCH_MAX_M):
            continue
        items.append({"obj": o, "hull": hull, "extent": extent, "level": o["position"][2],
                      "tris": [tuple(world[i][:2] for i in t) for t in mesh[1]] if mesh else None,
                      "plane": is_water_surface(o) and extent > PLANE_M})
    return items


def landmark_pieces(o, kind, groff, note):
    """The (kind, quad) pieces one constructed object contributes; note() says why it contributes none."""
    handle = int(o["object_section"], 16)
    mesh = geometry_mesh(groff, handle)
    pts = mesh[0] if mesh else geometry_points(groff, handle)
    if not pts:
        note("no geometry")
        return []
    world = world_points(o, pts)
    hull, _ = hull_of(world)
    if not hull:
        note("no footprint")
        return []
    quad = oriented_rect(hull)
    length, width = max(rect_sides(quad)), max(min(rect_sides(quad)), 0.01)
    floor = GATE_MIN_M if kind == GATE else (LINEAR_MIN_M if LINEAR_NAME.search(o["name"] or "") else LANDMARK_MIN_M)
    if length < floor:
        note("too small (%.1f m)" % length)
        return []
    if length > LANDMARK_MAX_M:
        note("too large (%.0f m)" % length)
        return []
    if kind == GATE and length > GATE_MAX_M:
        kind = BUILDING
    if mesh and kind == BUILDING and length >= HOLLOW_MIN_M:
        tris = [tuple(world[i][:2] for i in t) for t in mesh[1]]
        fill = covered_fraction(tris, hull)
        if fill < SOLID_FILL:
            lines = wall_lines(world, mesh[1])
            if lines:
                note("hollow (%.0f%% filled): %d wall lines" % (fill * 100, len(lines)))
                return [(WALL, line_quad(p, q)) for p, q in lines]
    if kind == BUILDING and width <= WALL_MAX_WIDTH_M and (length / width >= WALL_ASPECT or LINEAR_NAME.search(o["name"] or "")):
        kind = WALL
    note("%s %.0f x %.0f m" % (KIND_NAMES[kind], length, width))
    return [(kind, widen_rect(quad))]


def vehicle_marker(o, groff):
    """(x, y, axis x, axis y) of a vehicle body: its centre and the unit direction of its long side."""
    handle = int(o["object_section"], 16)
    mesh = geometry_mesh(groff, handle)
    pts = mesh[0] if mesh else geometry_points(groff, handle)
    hull, _ = hull_of(world_points(o, pts)) if pts else (None, 0.0)
    if hull:
        cx, cy, ux, uy, _hl, _hw = piece_frame(oriented_rect(hull))
        return cx, cy, ux, uy
    yaw = o["rotation"][2]
    return o["position"][0], o["position"][1], math.cos(yaw), math.sin(yaw)


def merge_vehicles(markers):
    """One marker per vehicle: bodies within VEHICLE_MERGE_M of an earlier one are dropped."""
    kept = []
    for m in markers:
        if all(math.hypot(m[0] - k[0], m[1] - k[1]) >= VEHICLE_MERGE_M for k in kept):
            kept.append(m)
    return kept


def build_level(data_dir, level, report=None):
    folder = minimap_bounds.level_data(data_dir, level)
    grf_path = next(p for p in folder.iterdir() if p.name.lower() == (level + ".grf").lower())
    groff = Groff(grf_path)
    res = analyse_level(folder, level)
    bounds = minimap_bounds.bounds_for(data_dir, level)
    span_x = bounds["max_x"] - bounds["min_x"]
    span_y = bounds["max_y"] - bounds["min_y"]
    rows = MASK_H
    cols = max(1, min(MAX_COLS, int(round(rows * span_x / span_y))))
    grid = Grid(cols, rows, bounds)
    objects = res["objects"]

    def note_for(o):
        return (lambda text: report.append((o["name"], o["class"], text))) if report is not None else (lambda text: None)

    auth = bytearray(cols * rows)
    sources = water_surfaces(groff, objects)
    for item in sources:
        if item["plane"]:
            cells = grid.cover_triangles(grid.plane, item["tris"]) if item["tris"] else grid.cover_hull(grid.plane, item["hull"])
            for c, r in cells:
                grid.plane_z[r * cols + c] = item["level"]
        elif item["tris"]:
            grid.cover_triangles(auth, item["tris"])
        else:
            grid.cover_hull(auth, item["hull"])

    land_objects = [o for o in objects if is_land_object(o)]
    for o in land_objects:
        x, y, z = o["position"]
        c, r = grid.cell_of(x, y)
        under = grid.inside(c, r) and grid.plane[r * cols + c]
        if under and z < grid.plane_z[r * cols + c] - SUBMERGED_M:
            continue
        grid.stamp_disk(grid.land, x, y, LAND_STAMP_M)
        if under:
            grid.stamp_disk(grid.dry, x, y, DRY_CORE_M)
    for o in objects:
        if not is_terrain_patch(o):
            continue
        pts = geometry_points(groff, int(o["object_section"], 16))
        hull, extent = hull_of(world_points(o, pts)) if pts else (None, 0.0)
        if hull and extent <= LAND_PATCH_M:
            grid.cover_hull(grid.land, hull)
            grid.cover_hull(grid.dry, hull)
    drop_small_blobs(grid.land, cols, rows, LAND_MIN_CELLS)

    water = bytearray(cols * rows)
    for i in range(cols * rows):
        water[i] = 1 if auth[i] or (grid.plane[i] and not grid.dry[i]) else 0

    pieces = []
    for o in objects:
        kind = landmark_kind(o)
        if kind is not None:
            pieces.extend(landmark_pieces(o, kind, groff, note_for(o)))
    merged = [(WALL if min(rect_sides(q)) <= WALL_MAX_WIDTH_M else BUILDING, q)
              for q in merge_walls([q for k, q in pieces if k == WALL])]
    unique = {(k, tuple((round(x, 1), round(y, 1)) for x, y in q)) for k, q in merged + [p for p in pieces if p[0] != WALL]}
    landmarks = sorted(unique, key=lambda l: (l[0], sum(p[1] for p in l[1]), sum(p[0] for p in l[1]), l[1]))

    bodies = sorted(vehicle_marker(o, groff) + (vehicle_size(o["name"]),)
                    for o in objects if VEHICLE_BODY.match(o["name"] or "") and o.get("object_section"))
    vehicles = merge_vehicles([(round(x, 1), round(y, 1), round(ax, 3), round(ay, 3), size) for x, y, ax, ay, size in bodies])

    runs = sorted(to_runs(grid.land, cols, rows, LAND) + to_runs(water, cols, rows, WATER),
                  key=lambda r: (r[2], r[3], r[0]))
    return {
        "id": bounds["id"], "cols": cols, "rows": rows, "runs": runs, "playable": len(land_objects),
        "objects": len(objects), "landmarks": landmarks, "vehicles": vehicles,
        "water_sources": [(i["obj"]["name"], i["obj"]["class"], round(i["extent"]), round(i["level"], 1),
                           "plane" if i["plane"] else "surface") for i in sources],
    }


def emit(levels):
    lines = ["/* Generated by tools/minimap_map.py: the minimap's land (kind 1) and water (kind 2) runs per level, in map",
             " * cells over the level's world bounds (minimap_bounds.inc), and its landmark footprints (kind: 0 building, 1 wall,",
             " * 2 road, 3 gate; four corners in world metres) and its vehicles (centre, unit direction of the long side, size 0 small, 1 medium, 2 large). */"]
    for lv in levels:
        lines.append("static const minimap_run_t k_runs_%s[] = {" % lv["id"].lower())
        for i in range(0, len(lv["runs"]), 6):
            chunk = lv["runs"][i:i + 6]
            lines.append("    " + " ".join("{%d,%d,%d,%d}," % r for r in chunk))
        lines.append("};")
        lines.append("")
        if lv["landmarks"]:
            lines.append("static const minimap_landmark_t k_landmarks_%s[] = {" % lv["id"].lower())
            for kind, q in lv["landmarks"]:
                lines.append("    {{%s}, {%s}, %d}," % (",".join("%.1ff" % p[0] for p in q), ",".join("%.1ff" % p[1] for p in q), kind))
            lines.append("};")
            lines.append("")
    for lv in levels:
        if lv["vehicles"]:
            lines.append("static const minimap_vehicle_t k_vehicles_%s[] = {" % lv["id"].lower())
            for x, y, ax, ay, size in lv["vehicles"]:
                lines.append("    {%.1ff, %.1ff, %.3ff, %.3ff, %d}," % (x, y, ax, ay, size))
            lines.append("};")
            lines.append("")
    lines.append("static const minimap_level_map_t k_maps[MM_LEVEL_COUNT] = {")
    for lv in levels:
        lines.append('    { "%s", %d, %d, k_runs_%s, %d, %s, %d, %s, %d },' % (
            lv["id"], lv["cols"], lv["rows"], lv["id"].lower(), len(lv["runs"]),
            "k_landmarks_" + lv["id"].lower() if lv["landmarks"] else "NULL", len(lv["landmarks"]),
            "k_vehicles_" + lv["id"].lower() if lv["vehicles"] else "NULL", len(lv["vehicles"])))
    lines.append("};")
    OUT.write_text("\n".join(lines) + "\n", encoding="ascii")


def svg(level, out_dir):
    cols, rows = level["cols"], level["rows"]
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (
        cols * 4, rows * 4, cols, rows), '<rect width="%d" height="%d" fill="#0a0c0a"/>' % (cols, rows)]
    for start, length, y, kind in level["runs"]:
        parts.append('<rect x="%d" y="%d" width="%d" height="1" fill="%s"/>' % (
            start, y, length, "#30422e" if kind == LAND else "#2060c0"))
    parts.append("</svg>")
    (out_dir / ("%s.svg" % level["id"])).write_text("\n".join(parts), encoding="ascii")


def write_report(level, rows, out_dir):
    lines = ["# %s: water sources, then every constructed candidate and what became of it" % level["id"]]
    lines += ["water\t%s\t%s\t%d m\tlevel %s\t%s" % tuple(s) for s in level["water_sources"]]
    lines += ["landmark\t%s\t%s\t%s" % (n, c, why) for n, c, why in rows]
    (out_dir / ("%s.txt" % level["id"])).write_text("\n".join(lines) + "\n", encoding="ascii", errors="replace")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--data", type=Path, default=ROOT / "game" / "data")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--svg", type=Path)
    ap.add_argument("--report", type=Path, help="write one text file per level listing what produced each element")
    args = ap.parse_args(argv)
    if args.report:
        args.report.mkdir(parents=True, exist_ok=True)
    levels = []
    for name in minimap_bounds.available_levels(args.data):
        rows = [] if args.report else None
        lv = build_level(args.data, name, rows)
        levels.append(lv)
        if args.report:
            write_report(lv, rows, args.report)
    emit(levels)
    if args.svg:
        args.svg.mkdir(parents=True, exist_ok=True)
        for lv in levels:
            svg(lv, args.svg)
    if args.stats:
        for lv in levels:
            kinds = [sum(1 for l in lv["landmarks"] if l[0] == k) for k in (BUILDING, WALL, ROAD, GATE)]
            print("%-11s objects=%5d landmarks=%4d (b%d w%d r%d g%d) vehicles=%2d land=%4d water=%4d mask=%dx%d" % (
                lv["id"], lv["objects"], len(lv["landmarks"]), *kinds, len(lv["vehicles"]),
                sum(1 for r in lv["runs"] if r[3] == LAND), sum(1 for r in lv["runs"] if r[3] == WATER),
                lv["cols"], lv["rows"]))
    print("wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
