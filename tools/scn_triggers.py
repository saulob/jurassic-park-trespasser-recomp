#!/usr/bin/env python3
"""Offline dump of Trespasser level triggers, actions and teleports from the retail scene files.

Each level is a .scn save image (GROFF container) that names its .GRF; the .GRF (SZDD-compressed GROFF) holds the
object placements and the value table with every object's text properties. Writes <level>.json and <level>.txt per
level plus index.json to the output directory.

    py -3 -I tools/scn_triggers.py --out <dir> [--data game/data] [--hint-strings USA_hints.rc2] [level ...]
"""
import argparse
import json
import math
import re
import struct
import sys
import zlib
from pathlib import Path

LEVELS = ["be", "jr", "ij", "it", "lab", "as", "as2", "sum"]
GROFF_MAGIC = 0x0ACEBABE

ACTION_TYPES = [
    "VOICEOVER", "AMBIENT", "MUSIC", "FADE_MUSIC", "SHOW_OVERLAY", "SET_FOG", "SET_RENDERER", "SET_TERRAIN",
    "SET_IMAGECACHE", "SET_AI", "SET_PHYSICS", "SUBSTITUTE_MESH", "SET_DEPTHSORT", "SET_SKY", "SET_ALPHA_WATER",
    "ENABLE_WATER", "LOAD_LEVEL", "SET_ANIMATE_PROPERTIES", "TELEPORT", "SAVE_LEVEL", "MAGNET",
    "SET_ANIMATE_TEXTURE", "HIDESHOW", "SOUND_EFFECT", "WAKE_AI", "DELAY", "SCRIPTED_ANIMATION",
    "SET_VARIABLE_TRIGGER", "SET_HINT", "AUDIO_ENVIRONMENT", "SUBSTITUTE_AI", "END_GAME", "CONTROL_PLAYER",
    "AI_SYSTEM", "TEXT", "WATER_DISTURBANCE",
]
ACTION_SET_HINT = 28
ACTION_LOAD_LEVEL = 16
ACTION_END_GAME = 31
ACTION_SET_VARIABLE = 27
PROCESS_STYLES = ["ALL", "STEPORDER", "STEPRANDOM", "SEQUENCEORDER", "SEQUENCERANDOM", "SEQUENCEORDERLOOP",
                  "SEQUENCERANDOMLOOP"]
LOCATION_CONDITIONS = [
    "PlayerInTrigger", "PlayerEnterTrigger", "PlayerLeaveTrigger", "ObjectInTrigger", "ObjectEnterTrigger",
    "ObjectLeaveTrigger", "CreatureInTrigger", "CreatureEnterTrigger", "CreatureLeaveTrigger",
]
TRIGGER_KEYS = {
    "Class", "FireCount", "FireExpression", "Probability", "FireAtZero", "ResetFire", "ProcessStyle", "FireDelay",
    "RepeatPeriod", "BoundVol", "SequenceDelayMin", "SequenceDelayMax", "TriggerActivate", "PointTrigger",
    "CreatureEnterCount", "CreatureLeaveCount", "AlphaChannel", "Split", "Merge", "Wrap", "Visible", "Mesh",
    "Physics", "Tangible", "Moveable", "SequenceListenNames", "SequenceOrderNames", "SequenceEvalNowNames",
    "SequenceFalseTriggerName", "EvaluateAll", "Element1", "Element2", "SoundMaterial1", "SoundMaterial2",
    "MinVelocity", "MaxVelocity", "InitialState", "MinHighTime", "MaxHighTime", "MinLowTime", "MaxLowTime",
    "UseObject", "PickUpObject", "PutDownObject", "CreatureDie", "CreatureWake", "CreatureSleep",
    "CreatureCriticalDamage", "CreatureDamagePoints",
} | set(LOCATION_CONDITIONS)
TELEPORT_CLASSES = {"Teleport", "CTeleport"}
HINT_LINE = re.compile(r'IDS_STR_HINTS\s*\+\s*(\d+)\s+"(.*)"')
EXPR_SPLIT = re.compile(r"[\s&|^=()!]+")


def szdd_expand(data):
    if data[:8] != b"SZDD\x88\xf0\x27\x33":
        return data
    size = struct.unpack_from("<I", data, 10)[0]
    window = bytearray(b" " * 4096)
    wpos = 4096 - 16
    out = bytearray()
    i, n = 14, len(data)
    while i < n and len(out) < size:
        flags = data[i]
        i += 1
        for bit in range(8):
            if i >= n:
                break
            if flags & (1 << bit):
                c = data[i]
                i += 1
                out.append(c)
                window[wpos] = c
                wpos = (wpos + 1) & 4095
            else:
                if i + 1 >= n:
                    break
                src = data[i] | ((data[i + 1] & 0xF0) << 4)
                length = (data[i + 1] & 0x0F) + 3
                i += 2
                for k in range(length):
                    c = window[(src + k) & 4095]
                    out.append(c)
                    window[wpos] = c
                    wpos = (wpos + 1) & 4095
    if len(out) != size:
        raise ValueError("SZDD stream ended at %d of %d bytes" % (len(out), size))
    return bytes(out)


class Groff:
    """GROFF container: 48-byte file header, 32-byte section headers, symbol table at the end."""

    def __init__(self, path):
        self.path = Path(path)
        raw = self.path.read_bytes()
        self.compressed = raw[:4] == b"SZDD"
        self.data = szdd_expand(raw)
        d = self.data
        (self.magic, self.file_size, section_count, symtab_entries, symtab_size, symtab_offset, self.timestamp,
         self.flags, self.version, _, _, self.crc) = struct.unpack_from("<12I", d, 0)
        if self.magic != GROFF_MAGIC:
            raise ValueError("%s: bad magic 0x%08X" % (path, self.magic))
        self.symbols = {}
        off = symtab_offset
        for _ in range(symtab_entries):
            handle, length, _refs = struct.unpack_from("<3I", d, off)
            off += 12
            self.symbols[handle] = d[off:off + length - 1].decode("latin-1")
            off += length
        self.sections = []
        self.by_handle = {}
        for i in range(section_count):
            name_sym, sflags, offset, size, reloc_off, reloc_count, user_flags, handle = struct.unpack_from(
                "<8I", d, 48 + 32 * i)
            sec = {"name": self.symbols.get(name_sym), "offset": offset, "size": size, "user_flags": user_flags,
                   "handle": handle}
            self.sections.append(sec)
            self.by_handle[handle] = sec

    def section(self, name):
        for sec in self.sections:
            if sec["name"] == name:
                return sec
        return None

    def section_bytes(self, sec):
        return self.data[sec["offset"]:sec["offset"] + sec["size"]]


class Reader:
    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.p)[0]
        self.p += 4
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def f32(self):
        v = struct.unpack_from("<f", self.d, self.p)[0]
        self.p += 4
        return v

    def cstr(self):
        end = self.d.index(b"\0", self.p)
        s = self.d[self.p:end].decode("latin-1")
        self.p = end + 1
        return s

    def handle(self):
        h, refs = struct.unpack_from("<2I", self.d, self.p)
        self.p += 8
        return h, refs

    def handle_profile(self):
        return {"class": self.cstr(), "base": self.u32(), "next": self.u32(), "count": self.u32(),
                "in_use": self.u8()}


class ValueTable:
    def __init__(self, blob):
        r = Reader(blob)
        self.name = r.cstr()
        self.symtab_profile = r.handle_profile()
        self.symtab_name = r.cstr()
        self.symbols = {}
        for _ in range(r.u32()):
            name = r.cstr()
            handle, _refs = r.handle()
            self.symbols[handle] = name
        self.value_profile = r.handle_profile()
        self.records = {}
        for _ in range(r.u32()):
            vtype, scope, ref = r.u8(), r.u8(), r.u8()
            value_handle, _ = r.handle()
            symbol_handle, _ = r.handle()
            target_handle, _ = r.handle()
            if vtype in (1, 2):
                value = r.u8()
                if vtype == 1:
                    value = bool(value)
            elif vtype == 3:
                value = r.i32()
            elif vtype == 4:
                value = r.f32()
            elif vtype == 5:
                value = r.cstr()
            elif vtype == 6:
                value = [r.handle()[0] for _ in range(r.u32())]
            else:
                raise ValueError("value table record type %d at byte %d" % (vtype, r.p))
            key, _ = r.handle()
            self.records[key] = {"type": vtype, "scope": scope, "ref": ref, "value_handle": value_handle,
                                 "symbol": symbol_handle, "target": target_handle, "value": value}
        self.consumed = r.p
        self.size = len(blob)

    def tree(self, handle, depth=0):
        rec = self.records.get(handle)
        if rec is None or depth > 32:
            return None
        if rec["type"] != 6:
            v = rec["value"]
            if isinstance(v, float):
                v = round(v, 6)
            return v
        out = {}
        for child in sorted(rec["value"], key=lambda h: self.records.get(h, {}).get("value_handle", h)):
            crec = self.records.get(child)
            if crec is None:
                out["<missing 0x%X>" % child] = None
                continue
            key = self.symbols.get(crec["symbol"], "<sym 0x%X>" % crec["symbol"])
            base, n = key, 2
            while key in out:
                key = "%s#%d" % (base, n)
                n += 1
            out[key] = self.tree(child, depth + 1)
        return out

    def object_name(self, handle):
        rec = self.records.get(handle)
        return self.symbols.get(rec["symbol"]) if rec else None


def geometry_extents(groff, object_section_handle):
    sec = groff.by_handle.get(object_section_handle)
    if not sec or sec["size"] < 28:
        return None
    _name, geom_handle = struct.unpack_from("<2I", groff.section_bytes(sec), 0)
    gsec = groff.by_handle.get(geom_handle)
    if not gsec:
        return None
    g = groff.section_bytes(gsec)
    points, fmt, pivot = None, None, None
    if len(g) >= 40:
        px, py, pz, nv, nf, nvn, nw = struct.unpack_from("<3f4I", g, 0)
        if 40 + 12 * (nv + 2 * nf + nvn + nw) == len(g):
            points = [struct.unpack_from("<3f", g, 40 + 12 * i) for i in range(nv)]
            fmt, pivot = "geometry", (px, py, pz)
    box_source = "points"
    if points is None and len(g) >= 44:
        px, py, pz = struct.unpack_from("<3f", g, 12)
        npoints, _nverts, _nvptrs, nwrap, _npolys = struct.unpack_from("<5I", g, 24)
        if 44 + 12 * (npoints + nwrap) <= len(g):
            points = [struct.unpack_from("<3f", g, 44 + 12 * i) for i in range(npoints)]
            fmt, pivot = "mesh_heap", (px, py, pz)
            if nwrap:
                base = len(g) - 12 * nwrap
                points = [struct.unpack_from("<3f", g, base + 12 * i) for i in range(nwrap)]
                box_source = "wrap"
    if not points:
        return None
    lo = [min(p[a] for p in points) for a in range(3)]
    hi = [max(p[a] for p in points) for a in range(3)]
    half = [max(0.01, max(abs(p[a]) for p in points)) for a in range(3)]
    return {"format": fmt, "box_source": box_source, "vertices": len(points), "pivot": [round(v, 4) for v in pivot],
            "local_min": [round(v, 4) for v in lo], "local_max": [round(v, 4) for v in hi],
            "local_half_extents": [round(v, 4) for v in half]}


def rotation_matrix(rx, ry, rz):
    def rx_m(a):
        c, s = math.cos(a), math.sin(a)
        return [[1, 0, 0], [0, c, s], [0, -s, c]]

    def ry_m(a):
        c, s = math.cos(a), math.sin(a)
        return [[c, 0, -s], [0, 1, 0], [s, 0, c]]

    def rz_m(a):
        c, s = math.cos(a), math.sin(a)
        return [[c, s, 0], [-s, c, 0], [0, 0, 1]]

    def mul(a, b):
        return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]

    return mul(mul(rx_m(rx), ry_m(ry)), rz_m(rz))


def trigger_area(obj):
    props = obj["props"] or {}
    bound = props.get("BoundVol", 0)
    scale = obj["scale"]
    if bound == 0:
        r = round(scale, 4)
        return {"shape": "sphere", "bound_vol": 0, "radius": r, "center": obj["position"],
                "world_aabb_min": [round(c - r, 3) for c in obj["position"]],
                "world_aabb_max": [round(c + r, 3) for c in obj["position"]]}
    geom = obj.get("geometry")
    if bound == 1 and geom:
        half = [h * scale for h in geom["local_half_extents"]]
        m = rotation_matrix(*obj["rotation"])
        aabb_half = [sum(abs(m[i][j]) * half[i] for i in range(3)) for j in range(3)]
        return {"shape": "box", "bound_vol": 1, "center": obj["position"],
                "half_extents_local_axes": [round(v, 3) for v in half],
                "size_local_axes": [round(2 * v, 3) for v in half],
                "rotation_rad": obj["rotation"],
                "world_aabb_min": [round(c - h, 3) for c, h in zip(obj["position"], aabb_half)],
                "world_aabb_max": [round(c + h, 3) for c, h in zip(obj["position"], aabb_half)]}
    return {"shape": "unknown", "bound_vol": bound, "center": obj["position"]}


def expression_symbols(expr):
    out = []
    for tok in EXPR_SPLIT.split(expr or ""):
        if not tok:
            continue
        dynamic = tok.startswith("@")
        name = tok[1:] if dynamic else tok
        if name.upper() in ("TRUE", "FALSE"):
            continue
        out.append({"name": name, "dynamic": dynamic})
    return out


def array_values(node):
    if not isinstance(node, dict):
        return []
    keys = sorted(k for k in node if re.fullmatch(r"A\d\d", k))
    return [node[k] for k in keys]


def collect_actions(props):
    actions = []
    if not isinstance(props, dict):
        return actions
    keys = sorted(k for k in props if re.fullmatch(r"Action\d\d", k) and isinstance(props[k], dict))
    if keys:
        for k in keys:
            actions.append((k, props[k]))
    elif "ActionType" in props:
        actions.append(("<inline>", props))
    return actions


def describe_action(slot, node):
    at = node.get("ActionType")
    desc = {"slot": slot, "action_type": at,
            "action_name": ACTION_TYPES[at] if isinstance(at, int) and 0 <= at < len(ACTION_TYPES) else None}
    if slot == "<inline>":
        desc["params"] = {k: v for k, v in node.items() if k not in TRIGGER_KEYS and k != "ActionType"}
    else:
        desc["params"] = {k: v for k, v in node.items() if k != "ActionType"}
    return desc


def string_refs(node, names, path=""):
    out = []
    if isinstance(node, dict):
        for k, v in node.items():
            if k == "Class":
                continue
            out.extend(string_refs(v, names, path + "/" + k if path else k))
    elif isinstance(node, str) and node in names:
        out.append({"path": path, "name": node})
    return out


def brief(t):
    parts = ["%s @ %s" % (t["class"], fmt_vec(t["position"]))]
    if "area" in t:
        parts.append(fmt_area(t["area"]))
        parts.append("on " + "/".join(t["conditions"]))
        if t.get("TriggerActivate"):
            parts.append("activator=" + t["TriggerActivate"])
    if "elements" in t:
        parts.append("collision %s x %s" % tuple(t["elements"]))
    for k in ("SequenceListenNames", "SequenceOrderNames", "objects"):
        if t.get(k):
            parts.append("%s=%s" % (k, ",".join(t[k])))
    if "initial_value" in t:
        parts.append("Value=%s" % t["initial_value"])
    if t["fire_expression"]:
        parts.append("expr=" + t["fire_expression"])
    parts.append("fire=%s" % t["fire_count"])
    acts = ",".join("%s%s" % (a["action_name"], "(%s)" % a["params"]["HintID"] if "HintID" in a["params"] else "")
                    for a in t["actions"])
    if acts:
        parts.append("actions=" + acts)
    return "; ".join(parts)


def decode_trigger_state(scn, name, cls, has_expression):
    sec = scn.section(crc_name(name))
    if sec is None:
        return None
    b = scn.section_bytes(sec)
    off = 0
    if cls == "CSequenceTrigger" and len(b) >= 4:
        off = 4 + 4 * struct.unpack_from("<I", b, 0)[0]
    if has_expression:
        off += 64
    if len(b) < off + 28:
        return {"undecoded_hex": b.hex()}
    next_action, life, prob, fire_time, seq, next_action_time, next_fire_time = struct.unpack_from(
        "<3Ifi2f", b, off)
    return {"next_action": next_action, "life": life if life != 0xFFFFFFFF else -1,
            "probability_u32": "0x%08X" % prob, "fire_time": fire_time, "sequence_actions": bool(seq),
            "next_action_time": next_action_time, "next_fire_time": next_fire_time,
            "tail_hex": b[off + 28:].hex()}


def crc_name(name):
    return "%X" % zlib.crc32(name.encode("latin-1"))


def parse_scn(path):
    g = Groff(path)
    info = {"file": str(path), "groff_version": g.version, "sections": len(g.sections)}
    hdr = g.section("Header")
    if hdr:
        b = g.section_bytes(hdr)
        r = Reader(b)
        info["save_version"] = r.i32()
        groffs = []
        for _ in range(5):
            raw = b[r.p:r.p + 256]
            r.p += 256
            s = raw.split(b"\0", 1)[0].decode("latin-1")
            if s:
                groffs.append(s)
        info["groff_files"] = groffs
        info["brief"] = bool(b[r.p])
        info["header_size"] = len(b)
    return g, info


def find_file(data_dir, stem, exts):
    lower = {p.name.lower(): p for p in data_dir.iterdir()}
    for ext in exts:
        p = lower.get((stem + ext).lower())
        if p:
            return p
    return None


def analyse_level(data_dir, level):
    scn_path = find_file(data_dir, level, [".scn"])
    if scn_path is None:
        raise FileNotFoundError("%s.scn not found in %s" % (level, data_dir))
    scn, scn_info = parse_scn(scn_path)
    grf_name = None
    for s in scn_info.get("groff_files", []):
        grf_name = Path(s.replace("\\", "/")).name
        break
    grf_path = find_file(data_dir, Path(grf_name).stem, [Path(grf_name).suffix, ".grf"]) if grf_name else None
    if grf_path is None:
        grf_path = find_file(data_dir, level, [".grf"])
    grf = Groff(grf_path)
    hdr = grf.section_bytes(grf.section(".header"))
    groff_magic, groff_ver = struct.unpack_from("<2I", hdr, 0)
    region = grf.section_bytes(grf.section(".region"))
    count = struct.unpack_from("<I", region, 0)[0]
    attr_section = struct.unpack_from("<I", region, 4 + count * 44)[0]
    vt = ValueTable(grf.section_bytes(grf.by_handle[attr_section]))

    scn_state = {}
    for sec in scn.sections:
        scn_state[sec["name"]] = sec["size"]

    objects = []
    for i in range(count):
        sec_h, sym_h, px, py, pz, rx, ry, rz, scale, attr_h, attr_refs = struct.unpack_from("<2I7f2I", region,
                                                                                           4 + i * 44)
        name = grf.symbols.get(sym_h)
        props = vt.tree(attr_h) if attr_h and vt.records.get(attr_h, {}).get("type") == 6 else None
        obj = {"index": i, "name": name, "class": props.get("Class") if isinstance(props, dict) else None,
               "position": [round(px, 4), round(py, 4), round(pz, 4)],
               "rotation": [round(rx, 6), round(ry, 6), round(rz, 6)], "scale": round(scale, 6),
               "object_section": "0x%08X" % sec_h, "value_handle": attr_h,
               "value_table_name": vt.object_name(attr_h) if props is not None else None, "props": props}
        if name and crc_name(name) in scn_state:
            obj["scn_state_bytes"] = scn_state[crc_name(name)]
        objects.append(obj)

    names = {}
    for o in objects:
        if o["name"]:
            names.setdefault(o["name"], []).append(o["index"])

    triggers = {}
    for o in objects:
        cls = o["class"] or ""
        if not cls.endswith("Trigger"):
            continue
        p = o["props"] or {}
        if cls == "CLocationTrigger" or "BoundVol" in p:
            o["geometry"] = geometry_extents(grf, int(o["object_section"], 16))
        t = {"name": o["name"], "class": cls, "index": o["index"], "position": o["position"],
             "rotation": o["rotation"], "scale": o["scale"],
             "fire_count": p.get("FireCount", -1), "fire_expression": p.get("FireExpression"),
             "process_style": PROCESS_STYLES[p.get("ProcessStyle", 0)]
             if 0 <= p.get("ProcessStyle", 0) < len(PROCESS_STYLES) else p.get("ProcessStyle"),
             "fire_delay": p.get("FireDelay"), "probability": p.get("Probability"),
             "fire_at_zero": p.get("FireAtZero"), "reset_fire": p.get("ResetFire")}
        if cls == "CLocationTrigger":
            t["area"] = trigger_area(o)
            conds = [c for c in LOCATION_CONDITIONS if p.get(c)]
            t["conditions"] = conds or ["PlayerEnterTrigger (default)"]
            for k in ("TriggerActivate", "PointTrigger", "CreatureEnterCount", "CreatureLeaveCount"):
                if k in p:
                    t[k] = p[k]
        if cls == "CSequenceTrigger":
            for k in ("SequenceListenNames", "SequenceOrderNames", "SequenceEvalNowNames"):
                if k in p:
                    t[k] = array_values(p[k])
            if "SequenceFalseTriggerName" in p:
                t["SequenceFalseTriggerName"] = p["SequenceFalseTriggerName"]
        if cls == "CVariableTrigger":
            t["initial_value"] = p.get("Value", False)
        if cls == "CObjectTrigger":
            t["objects"] = [p[k] for k in sorted(p) if re.fullmatch(r"A\d\d", k)]
            for k in ("UseObject", "PickUpObject", "PutDownObject"):
                if k in p:
                    t[k] = p[k]
        if cls == "CCreatureTrigger" or any(k.startswith("Creature") for k in p):
            for k in p:
                if k.startswith("Creature"):
                    t[k] = p[k]
        if cls == "CCollisionTrigger":
            t["elements"] = [p.get("Element1", "<any>"), p.get("Element2", "<any>")]
        t["trigger_props"] = {k: v for k, v in p.items() if k != "Class" and not isinstance(v, dict)}
        t["warnings"] = ["unrecognised key %r (engine reads BoundVol)" % k for k in p
                         if k.lower().startswith("bound") and k != "BoundVol"]
        t["expression_symbols"] = expression_symbols(t["fire_expression"])
        for s in t["expression_symbols"]:
            if s["name"] not in names:
                t["warnings"].append("expression symbol %r not in level" % s["name"])
        t["actions"] = [describe_action(s, n) for s, n in collect_actions(p)]
        t["string_refs"] = string_refs(p, names)
        if "scn_state_bytes" in o:
            t["scn_state_bytes"] = o["scn_state_bytes"]
            t["scn_state"] = decode_trigger_state(scn, o["name"], cls, bool(t["fire_expression"]))
        triggers[o["name"]] = t

    depends, affects = {}, {}
    for name, t in triggers.items():
        dep = set(s["name"] for s in t["expression_symbols"])
        for k in ("SequenceListenNames", "SequenceOrderNames", "SequenceEvalNowNames"):
            dep.update(t.get(k, []))
        if t.get("SequenceFalseTriggerName"):
            dep.add(t["SequenceFalseTriggerName"])
        aff = set()
        for a in t["actions"]:
            if a["action_type"] == ACTION_SET_VARIABLE and a["params"].get("TriggerName"):
                aff.add(a["params"]["TriggerName"])
        depends[name] = sorted(dep)
        affects[name] = sorted(aff)
    acted_on = {}
    for name, t in triggers.items():
        for a in t["actions"]:
            for v in a["params"].values():
                if isinstance(v, str):
                    acted_on.setdefault(v, set()).add(name)
    for name, t in triggers.items():
        t["activator_affected_by"] = sorted(acted_on.get(t.get("TriggerActivate"), set()) - {name})
        t["depends_on"] = depends[name]
        t["sets_variables"] = affects[name]
        t["depended_on_by"] = sorted(n for n, d in depends.items() if name in d)
        t["set_by"] = sorted(n for n, a in affects.items() if name in a)
        t["referenced_by"] = sorted(set(n for n, x in triggers.items() for r in x["string_refs"]
                                        if r["name"] == name and n != name))

    def upstream(name, depth):
        seen, frontier, chain = set(), [name], []
        for level_no in range(depth):
            nxt = []
            for n in frontier:
                t = triggers.get(n)
                if not t:
                    continue
                for d in t["depends_on"] + t["set_by"] + t["activator_affected_by"]:
                    if d not in seen and d != name:
                        seen.add(d)
                        nxt.append(d)
                        chain.append({"name": d, "distance": level_no + 1, "via": n,
                                      "class": triggers[d]["class"] if d in triggers else
                                      (objects[names[d][0]]["class"] if d in names else "<not in level>"),
                                      "summary": brief(triggers[d]) if d in triggers else ""})
            frontier = nxt
        return chain

    hints, exits, set_vars = [], [], []
    for name, t in triggers.items():
        for a in t["actions"]:
            entry = {"trigger": name, "class": t["class"], "position": t["position"], "slot": a["slot"],
                     "fire_count": t["fire_count"], "fire_expression": t["fire_expression"],
                     "process_style": t["process_style"]}
            if "area" in t:
                entry["area"] = t["area"]
                entry["conditions"] = t["conditions"]
            entry["summary"] = brief(t)
            entry["trigger_props"] = t["trigger_props"]
            entry["action_params"] = a["params"]
            entry["warnings"] = t["warnings"]
            if a["action_type"] == ACTION_SET_HINT:
                entry["hint_id"] = a["params"].get("HintID")
                hints.append(entry)
            elif a["action_type"] in (ACTION_LOAD_LEVEL, ACTION_END_GAME):
                entry["action"] = a["action_name"]
                entry["level_name"] = a["params"].get("LevelName")
                exits.append(entry)
            elif a["action_type"] == ACTION_SET_VARIABLE:
                entry["params"] = a["params"]
                set_vars.append(entry)
    for e in hints + exits:
        t = triggers[e["trigger"]]
        e["depends_on"] = t["depends_on"]
        e["set_by"] = t["set_by"]
        e["depended_on_by"] = t["depended_on_by"]
        e["sets_variables"] = t["sets_variables"]
        e["referenced_by"] = t["referenced_by"]
        e["activator_affected_by"] = t["activator_affected_by"]
        e["scn_state"] = t.get("scn_state")
        e["upstream"] = upstream(e["trigger"], 16)
    hints.sort(key=lambda e: (e["hint_id"] if isinstance(e["hint_id"], int) else 1 << 30, e["trigger"]))

    teleports = [{"index": o["index"], "name": o["name"], "position": o["position"], "rotation": o["rotation"]}
                 for o in objects if o["class"] in TELEPORT_CLASSES]
    classes = {}
    for o in objects:
        classes[o["class"] or "<none>"] = classes.get(o["class"] or "<none>", 0) + 1

    return {
        "level": level,
        "scn": scn_info,
        "grf": {"file": str(grf_path), "szdd": grf.compressed, "decompressed_size": len(grf.data),
                "container_version": grf.version, "sections": len(grf.sections),
                "groff_header_magic": "0x%08X" % groff_magic, "groff_version": groff_ver & 0x7FFFFFFF,
                "optimized": bool(groff_ver & 0x80000000), "object_count": count,
                "value_table": {"records": len(vt.records), "symbols": len(vt.symbols),
                                "bytes_consumed": vt.consumed, "bytes": vt.size}},
        "class_counts": dict(sorted(classes.items(), key=lambda kv: -kv[1])),
        "hints": hints,
        "level_exits": exits,
        "variable_sets": set_vars,
        "teleports": teleports,
        "triggers": triggers,
        "objects": objects,
    }


def fmt_vec(v):
    return "(" + ", ".join("%.2f" % x for x in v) + ")"


def fmt_area(a):
    if not a:
        return ""
    if a["shape"] == "sphere":
        return "sphere r=%.2f" % a["radius"]
    if a["shape"] == "box":
        return "box %s (aabb %s..%s)" % ("x".join("%.2f" % v for v in a["size_local_axes"]),
                                         fmt_vec(a["world_aabb_min"]), fmt_vec(a["world_aabb_max"]))
    return "BoundVol=%s (no geometry)" % a.get("bound_vol")


def progression_lines(w, e):
    w("      %s" % e["summary"])
    w("      slot=%s style=%s action_params=%s" % (e["slot"], e["process_style"],
                                                  json.dumps(e["action_params"], ensure_ascii=False)))
    for x in e["warnings"]:
        w("      WARNING: %s" % x)
    if e["depended_on_by"]:
        w("      used by: %s" % ", ".join(e["depended_on_by"]))
    if e["referenced_by"]:
        w("      referenced by: %s" % ", ".join(e["referenced_by"]))
    if e["sets_variables"]:
        w("      sets variables: %s" % ", ".join(e["sets_variables"]))
    for u in e["upstream"]:
        w("      %s<- %s (via %s): %s" % ("  " * (u["distance"] - 1), u["name"], u["via"], u["summary"] or u["class"]))


def text_report(res):
    out = []
    w = out.append
    w("Level %s" % res["level"])
    w("  scn: %s (save version %s, groff %s)" % (res["scn"]["file"], res["scn"].get("save_version"),
                                                 res["scn"].get("groff_files")))
    g = res["grf"]
    w("  grf: %s objects=%d value records=%d (%d/%d bytes)" % (g["file"], g["object_count"],
                                                               g["value_table"]["records"],
                                                               g["value_table"]["bytes_consumed"],
                                                               g["value_table"]["bytes"]))
    w("")
    w("HINTS (ActionType 28)")
    for h in res["hints"]:
        w("  HintID %-4s %s" % (h["hint_id"], h["trigger"]))
        if h.get("hint_text"):
            w("      text: %s" % h["hint_text"])
        progression_lines(w, h)
    w("")
    w("LEVEL EXITS (ActionType 16 LOAD_LEVEL / 31 END_GAME)")
    for e in res["level_exits"]:
        w("  %s -> %s  %s" % (e["action"], e["level_name"], e["trigger"]))
        progression_lines(w, e)
    w("")
    w("TELEPORTS (CTeleport, file order) count=%d" % len(res["teleports"]))
    for t in res["teleports"]:
        w("  %-32s %s rotZ=%.3f" % (t["name"], fmt_vec(t["position"]), t["rotation"][2]))
    w("")
    w("TRIGGERS")
    for name, t in sorted(res["triggers"].items(), key=lambda kv: kv[1]["index"]):
        w("  %s [%s] pos %s scale=%.3f fire=%s style=%s %s" % (
            name, t["class"], fmt_vec(t["position"]), t["scale"], t["fire_count"], t["process_style"],
            fmt_area(t.get("area"))))
        if t.get("conditions"):
            w("      conditions: %s%s" % (", ".join(t["conditions"]),
                                         ("  activate=" + t["TriggerActivate"]) if t.get("TriggerActivate") else ""))
        if t["fire_expression"]:
            w("      expr: %s" % t["fire_expression"])
        for k in ("SequenceListenNames", "SequenceOrderNames", "SequenceEvalNowNames", "SequenceFalseTriggerName",
                  "objects", "initial_value"):
            if k in t:
                w("      %s: %s" % (k, t[k]))
        for a in t["actions"]:
            w("      %s: %s(%s) %s" % (a["slot"], a["action_name"], a["action_type"],
                                      json.dumps(a["params"], ensure_ascii=False)))
        if t["depended_on_by"]:
            w("      used by: %s" % ", ".join(t["depended_on_by"]))
        if t["set_by"]:
            w("      set by: %s" % ", ".join(t["set_by"]))
    w("")
    w("CLASS COUNTS")
    for k, v in res["class_counts"].items():
        w("  %-28s %d" % (k, v))
    return "\n".join(out) + "\n"


def main(argv=None):
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("levels", nargs="*", default=LEVELS)
    ap.add_argument("--data", type=Path, default=root / "game" / "data")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--hint-strings", type=Path, help="optional .rc2 string table with IDS_STR_HINTS + N lines")
    args = ap.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    hint_text = {}
    if args.hint_strings:
        for line in args.hint_strings.read_text(encoding="utf-8", errors="replace").splitlines():
            m = HINT_LINE.search(line)
            if m:
                hint_text[int(m.group(1))] = m.group(2)
    index = {}
    for level in args.levels:
        res = analyse_level(args.data, level)
        for h in res["hints"]:
            if h["hint_id"] in hint_text:
                h["hint_text"] = hint_text[h["hint_id"]]
        (args.out / ("%s.json" % level)).write_text(json.dumps(res, indent=1, ensure_ascii=False), encoding="utf-8")
        (args.out / ("%s.txt" % level)).write_text(text_report(res), encoding="utf-8")
        index[level] = {"hints": [(h["hint_id"], h["trigger"]) for h in res["hints"]],
                        "exits": [(e["action"], e["level_name"], e["trigger"]) for e in res["level_exits"]],
                        "teleports": [t["name"] for t in res["teleports"]],
                        "objects": res["grf"]["object_count"], "triggers": len(res["triggers"])}
        print("%-4s objects=%d triggers=%d hints=%d exits=%d teleports=%d" % (
            level, res["grf"]["object_count"], len(res["triggers"]), len(res["hints"]), len(res["level_exits"]),
            len(res["teleports"])))
    (args.out / "index.json").write_text(json.dumps(index, indent=1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
