#!/usr/bin/env python3
"""The structure offsets src/host hard-codes, and what is proven of each in the other builds.

    py -3 tools/host_offsets.py [--write] [--moves <id> <moves.json>]

--moves stores config/layout_moves/<id>.json, the catalogue of displacements tools/layout_diff.py finds changed in aligned code.
--write rewrites config/host_offsets.json: one row per `#define NAME 0x..` (or `#define NAME GLAYOUT_..`) of src/host that is a small
offset (excluded: config/host_offsets_exclude.json) and one row per raw `+ 0x..` literal inside a guest memory accessor
(`@0x..`, not the ones config/host_literals_allow.json classifies as host buffers, file formats, stack arguments or types).
Per profile, a status:
  unmoved    the same field in that build: the code of the structure's class or set (config/layout_structs.json, evidence from
             tools/class_layout.py) accesses that displacement with identical or aligned instructions and nothing moves it; an
             array's stride is built by the same address arithmetic; a table is byte-identical; a hooked function's frame or
             code is identical; or, for a raw literal in a file that overrides guest functions, those functions agree
  moved      the field has another offset in that build (value); the host reads it through a GLAYOUT_ descriptor
  not_guest  not a guest structure offset (COM vtable slot, file format, host constant, the Smacker DLL's own structure)
  gated      every use is in code that runs only when a patch group the profile does not apply is on
  unproven   no proof: the offset is not classified, or the evidence does not show it unmoved
Decisions that need a human reading of the code are in config/host_offsets_decisions.json.
Without options it checks the manifest against the sources and exits 1 when it is out of date.
"""
import argparse
import fnmatch
import json
import re
import sys

from common import CONFIG_DIR, ROOT

MANIFEST = CONFIG_DIR / "host_offsets.json"
EXCLUDE = CONFIG_DIR / "host_offsets_exclude.json"
DECISIONS = CONFIG_DIR / "host_offsets_decisions.json"
STRUCTS = CONFIG_DIR / "layout_structs.json"
MOVES = CONFIG_DIR / "layout_moves"
EVIDENCE = CONFIG_DIR / "layout_evidence"
DEFINE = re.compile(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]*)\s+\(?0x([0-9A-Fa-f]{1,4})[uU]?\)?\s*(?:/\*.*)?$")
DESCRIPTOR_DEFINE = re.compile(r"^\s*#\s*define\s+([A-Z][A-Z0-9_]*)\s+(GLAYOUT_[A-Z0-9_]+)\s*(?:/\*.*)?$")
DESCRIPTORS = CONFIG_DIR / "layout_descriptors.json"
ACCESSOR = re.compile(r"\b(MEM8|MEM16|MEM32|MEMF|ADDR|guest_read\w*|guest_write\w*|guest_readable|guest_writable|rd|rdf|rd32|wr|wrf|"
                      r"wr32|read_vec|write_vec)\s*\(")
LITERAL = re.compile(r"\+\s*\(?\s*(0[xX][0-9A-Fa-f]+|\d+)[uU]?\b(?!\s*[*\w])")
LITERAL_ALLOW = CONFIG_DIR / "host_literals_allow.json"
PROFILES = ("es-117", "ptbr-117")


GROUP_FILES = {"src/host/patch117_player.c": "hand_jump", "src/host/patch117_physics.c": "box_box"}
# (group, first line of the code, line that starts its last function): code that runs only in a program that applies the group
GROUP_REGIONS = {"src/host/patch117_physics.c": ("wake", "static patch117_wake_box_t wake_read(", "void GHOOK(0x004DDEFD)(void) {")}


def group_conditions(path):
    """Per source line, the patch groups whose code the line belongs to: it runs only in a program that applies them all
    (the host installs a group's code only where the lift applies the group, and the lift calls its hooks only there)."""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    out = {n: [] for n in range(1, len(lines) + 1)}
    rel = path.relative_to(ROOT).as_posix()
    if rel in GROUP_FILES:
        for n in out:
            out[n].append(GROUP_FILES[rel])
    if rel in GROUP_REGIONS:
        group, first, last = GROUP_REGIONS[rel]
        start = next(n for n, line in enumerate(lines, 1) if line.startswith(first))
        tail = next(n for n, line in enumerate(lines, 1) if line.startswith(last))
        end = next(n for n in range(tail, len(lines) + 1) if lines[n - 1] == "}")
        for n in range(start, end + 1):
            out[n].append(group)
    return out


def compiled_out(rel, lines, groups):
    """True when none of `lines` of the source runs in a program that applies exactly `groups`."""
    if not (ROOT / rel).exists():
        return False
    conditions = group_conditions(ROOT / rel)
    return bool(lines) and all(any(not groups[g] for g in conditions[n]) for n in lines)


def define_uses(rel, name):
    uses = []
    if not (ROOT / rel).exists():
        return uses
    for n, line in enumerate((ROOT / rel).read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        code = line.split("//")[0]
        if re.search(r"\b" + name + r"\b", code) and not re.match(r"^\s*#\s*define\s+" + name + r"\b", code):
            uses.append(n)
    return uses


def host_defines():
    exclude = {(e["file"], e["name"]) for e in json.loads(EXCLUDE.read_text(encoding="utf-8"))["names"]} if EXCLUDE.exists() else set()
    rows = []
    descriptors = {f"GLAYOUT_{d['name']}": d["value"] for d in json.loads(DESCRIPTORS.read_text(encoding="utf-8"))["descriptors"]}
    for path in sorted((ROOT / "src" / "host").rglob("*")):
        rel = path.relative_to(ROOT).as_posix()
        if path.suffix not in (".c", ".h", ".cpp") or "/profiles/" in rel:
            continue
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = DEFINE.match(line)
            if m and 4 <= int(m.group(2), 16) < 0x10000 and (rel, m.group(1)) not in exclude:
                rows.append({"file": rel, "name": m.group(1), "value": f"0x{int(m.group(2), 16):X}"})
            d = DESCRIPTOR_DEFINE.match(line)
            if d and d.group(2) in descriptors:
                rows.append({"file": rel, "name": d.group(1), "value": f"0x{int(descriptors[d.group(2)], 16):X}",
                             "descriptor": d.group(2)})
    return rows


def host_literals():
    """One row per (file, displacement) of the raw literals `+ N` inside guest memory accessors: reading the guest at
    `base + 0x58` hard-codes a structure offset that no name documents. `+ 4u * i` is an array of 32-bit values."""
    rows = {}
    allow = json.loads(LITERAL_ALLOW.read_text(encoding="utf-8"))["allow"] if LITERAL_ALLOW.exists() else []
    for path in sorted((ROOT / "src" / "host").rglob("*.c")):
        rel = path.relative_to(ROOT).as_posix()
        if "/profiles/" in rel:
            continue
        skipped = [re.compile(a["match"]) for a in allow if a["file"] in ("*", path.name)]
        text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), path.read_text(encoding="utf-8", errors="replace"), flags=re.S)
        for n, line in enumerate(text.splitlines(), 1):
            line = line.split("//")[0]
            if ACCESSOR.search(line) and not any(m.search(line) for m in skipped):
                for m in LITERAL.finditer(line):
                    value = int(m.group(1), 0)
                    if 4 <= value < 0x10000:
                        row = rows.setdefault((rel, value), {"file": rel, "name": f"@{value:#x}", "value": f"0x{value:X}", "literal": True,
                                                              "lines": []})
                        row["lines"].append(n)
    return [rows[k] for k in sorted(rows)]


def store_moves(profile, source):
    raw = json.loads(open(source, encoding="utf-8").read())
    out = {}
    for key, tos in raw.items():
        kind, value = key.split(":")
        for to, funcs in tos.items():
            out.setdefault(f"{kind}:{int(value, 16):#x}", {})[f"{int(to, 16):#x}"] = len(funcs)
    MOVES.mkdir(parents=True, exist_ok=True)
    (MOVES / f"{profile}.json").write_text(json.dumps(out, indent=0, sort_keys=True) + "\n", encoding="utf-8")


def rule_for(rules, rel, name):
    best = None
    for rule in rules:
        if "files" in rule and rel.split("/")[-1] not in rule["files"]:
            continue
        for pattern in rule["names"]:
            if fnmatch.fnmatchcase(name, pattern):
                if best is None or ("*" not in pattern and "*" in best[1]):
                    best = (rule, pattern)
    return best[0] if best else None


def class_chain(rtti, name):
    c = rtti["classes"].get(name)
    return [name] + (c["bases"] if c else [])


def dll_identical():
    digests = set()
    for path in (ROOT / "launcher").glob("game_manifest_*.inc"):
        m = re.search(r'"SMACKW32\.DLL", (\d+), "([0-9a-f]{64})"', path.read_text(encoding="utf-8"))
        digests.add((m.group(1), m.group(2)) if m else None)
    return len(digests) == 1 and None not in digests


def class_status(evidence, chain, value, kind="field"):
    classes = [evidence["classes"][c] for c in chain if c in evidence["classes"]]
    if not classes:
        return "unproven", None, f"{chain[0]} has no evidence"
    if kind == "bytes":
        info = classes[0].get("bytes")
        if info and info["equal"]:
            return "unmoved", None, f"{chain[0]}: the {info['length']}-byte table is byte-identical at its mapped address"
        return "unproven", None, f"{chain[0]}: the table bytes differ or are not mapped"
    if kind == "code":
        c = classes[0]
        if c["functions"] and c["changed"] == 0 and c["unmatched"] == 0:
            return "unmoved", None, f"{chain[0]}: the {c['functions']} functions the host reads inside are byte-identical, so their registers and frame are"
        return "unproven", None, f"{chain[0]}: a function the host reads inside changed"
    if kind == "frame":
        frame = classes[0].get("frame")
        if frame and frame["other"] == frame["engine"] == value:
            return "unmoved", None, f"{chain[0]}: the entry sequence of the hooked function reserves {value:#x} bytes in both builds"
        return "unproven", None, f"{chain[0]}: the entry sequence reserves {frame and frame['engine']:#x} and {frame and frame['other']}"
    if kind == "slot":
        marks = classes[0].get("slots", "")
        index = value // 4
        where = f"{chain[0]} vtable"
        if index < len(marks) and marks[index] == "y":
            return "unmoved", None, f"{where}: slot {index} holds a function matched by its code in both builds"
        return "unproven", None, f"{where}: slot {index} is not matched by code"
    strides = classes[0].get("strides") if kind == "size" else None
    if strides and f"{value:#x}" in strides["engine"]:
        here = strides["engine"][f"{value:#x}"]
        there = strides["other"] or {}
        if f"{value:#x}" in there:
            return "unmoved", None, (f"{chain[0]}: the array's stride {value:#x} is built by the same address arithmetic "
                                     f"({here} accesses in 1.0.116, {there[f'{value:#x}']} here)")
        became = [k for k, n in there.items() if k not in strides["engine"] and n * 2 >= here]
        if len(became) == 1:
            return "moved", [became[0]], (f"{chain[0]}: the array's stride {value:#x} ({here} accesses) is {became[0]} here "
                                          f"({there[became[0]]} accesses)")
    table = classes[0].get("bytes")
    if kind == "size" and table and table["equal"]:
        return "unmoved", None, f"{chain[0]}: the {table['length']}-byte table is byte-identical at its mapped address, so its rows are"
    prefixes = ("i:",) if kind == "size" else ("d:", "i:") if kind == "member" else ("d:",)
    keys = [p + f"{value:#x}" for p in prefixes]
    identical = sum(c["access"].get(k, [0, 0, 0])[0] for c in classes for k in keys)
    same = sum(c["access"].get(k, [0, 0, 0])[1] for c in classes for k in keys)
    other = sum(c["access"].get(k, [0, 0, 0])[2] for c in classes for k in keys)
    moves = sorted({t for c in classes for k in keys for t in c["moves"].get(k, [])})
    targets = sorted({k for c in classes for k, ts in c["moves"].items() if k[:2] in prefixes and f"{value:#x}" in ts})
    functions = sum(c["functions"] for c in classes)
    unmatched = sum(c["unmatched"] for c in classes)
    where = f"{'+'.join(chain[:3])}: {functions} functions, {unmatched} without a counterpart"
    what = "constant" if kind == "size" else "+"
    if identical:
        note = f"; {other} realigned accesses disagree" if moves else ""
        return "unmoved", None, f"{where}; {what}{value:#x} used by {identical} identical functions{note}"
    if kind == "size" and sum(c["changed"] + c["unmatched"] for c in classes) == 0:
        return "unmoved", None, f"{where}; every function that indexes it is byte-identical"
    absolute = [c.get("abs") for c in classes if c.get("abs")]
    if kind in ("field", "member") and absolute:
        entry = absolute[0]["fields"].get(f"{value:#x}")
        if entry and entry[0] == f"{value:#x}":
            return "unmoved", None, f"{where}; the object's field at +{value:#x} is referenced by address ({entry[1]} aligned) and moves with the object"
    if moves:
        return "unproven", moves, f"{where}; {other} accesses change {what}{value:#x} to {', '.join(moves)}"
    if targets:
        return "unproven", None, f"{where}; {what}{value:#x} is where {', '.join(targets)} moves to"
    if identical + same:
        return "unmoved", None, f"{where}; {what}{value:#x} used by {identical} identical and {same} aligned functions, none moved"
    return "unproven", None, f"{where}; no function uses {what}{value:#x}"


def port_proof(evidence, rel, value):
    """A raw offset in a file that overrides guest functions: proven when those functions are byte-identical, or when the
    aligned instructions of the ones that changed read that displacement unmoved."""
    name = "port:" + rel.split("/")[-1]
    entry = evidence["classes"].get(name)
    if not entry or not entry["functions"] or entry["unmatched"]:
        return None
    if entry["changed"] == 0:
        return f"port: the {entry['functions']} guest functions {rel.split('/')[-1]} overrides are byte-identical, so is what they read"
    status, _, text = class_status(evidence, [name], value)
    return f"port: {text}" if status == "unmoved" else None


def build():
    decisions = json.loads(DECISIONS.read_text(encoding="utf-8"))["offsets"] if DECISIONS.exists() else []
    decided = {(d["file"], d["name"], d["profile"]): d for d in decisions}
    rules = json.loads(STRUCTS.read_text(encoding="utf-8"))["rules"]
    rtti = json.loads((ROOT / "analysis" / "rtti.json").read_text(encoding="utf-8"))
    evidence = {p: json.loads((EVIDENCE / f"{p}.json").read_text(encoding="utf-8")) for p in PROFILES}
    applied = {p: json.loads((CONFIG_DIR / "profiles" / f"{p}.json").read_text(encoding="utf-8"))["patch117"] for p in PROFILES}
    smack = dll_identical()
    rows = []
    for row in host_defines() + host_literals():
        value = int(row["value"], 16)
        rule = rule_for(rules, row["file"], row["name"])
        if rule and ("class" in rule or "set" in rule):
            row["struct"] = rule.get("class") or rule["set"]
        for profile in PROFILES:
            d = decided.get((row["file"], row["name"], profile))
            if compiled_out(row["file"], row.get("lines") or define_uses(row["file"], row["name"]), applied[profile]):
                row[profile] = {"status": "gated", "evidence": "every use is in code that runs only when a patch group this profile does not apply is on"}
            elif d:
                row[profile] = {"status": d["status"], "value": d.get("value", row["value"]), "evidence": d["evidence"]}
            elif rule and "category" in rule:
                ok = smack if rule["category"] == "DLL_STRUCT" else True
                row[profile] = ({"status": "not_guest", "evidence": f"{rule['category']}: {rule['reason']}"} if ok
                                else {"status": "unproven"})
            elif rule and ("class" in rule or "set" in rule):
                chain = class_chain(rtti, rule["class"]) if "class" in rule else [rule["set"]]
                status, moves, text = class_status(evidence[profile], chain, value, rule.get("kind", "field"))
                row[profile] = {"status": status, "evidence": text}
                if status == "moved":
                    row[profile]["value"] = moves[0]
                elif moves:
                    row[profile]["moves"] = moves
            elif row.get("literal") and port_proof(evidence[profile], row["file"], value):
                row[profile] = {"status": "unmoved", "value": row["value"], "evidence": port_proof(evidence[profile], row["file"], value)}
            else:
                row[profile] = {"status": "unproven"}
        rows.append(row)
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--moves", nargs=2, metavar=("PROFILE", "JSON"))
    args = ap.parse_args(argv)
    if args.moves:
        store_moves(*args.moves)
        return 0
    rows = build()
    if args.write:
        MANIFEST.write_text(json.dumps({"offsets": rows}, indent=1) + "\n", encoding="utf-8")
        for profile in PROFILES:
            counts = {}
            for r in rows:
                counts[r[profile]["status"]] = counts.get(r[profile]["status"], 0) + 1
            print(profile, counts)
        return 0
    stored = json.loads(MANIFEST.read_text(encoding="utf-8"))["offsets"] if MANIFEST.exists() else []
    stale = [r for r in rows if r not in stored]
    for r in stale:
        print("manifest out of date:", r["file"], r["name"], r["value"])
    return 1 if stale or len(stored) != len(rows) else 0


if __name__ == "__main__":
    import host_offsets
    sys.exit(host_offsets.main())
