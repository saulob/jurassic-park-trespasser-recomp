#!/usr/bin/env python3
"""Evidence for the layout of the structures src/host reads: what the code that owns a structure accesses in 1.0.116, and
whether the same instructions access the same offsets in another build.

    py -3 tools/class_layout.py <profile id> [--exe <trespass.exe>]

Writes config/layout_evidence/<id>.json for
  every RTTI class: the functions in its vtable and the functions that store its vtable;
  every set of config/layout_structs.json: explicit guest functions, or every function that loads the pointer held in a
  guest global and accesses [reg+O] through it ("global").
Per entry
  functions / identical / changed / unmatched   how its code compares with the other build
  access   {"d:<offset>" or "i:<constant>": [in identical functions, in changed functions with the same value, in changed
           functions with another value]}   (stack locals, esp- and negative-ebp-based, are not fields and are skipped)
  moves    {"d:<offset>": [the values it becomes]}
Read only.
"""
import argparse
import collections
import difflib
import json
import re
import sys
from pathlib import Path

from common import CONFIG_DIR, ROOT, TARGET
import guest_map

EVIDENCE = CONFIG_DIR / "layout_evidence"
STRUCTS = CONFIG_DIR / "layout_structs.json"
LOW, HIGH = 4, 0x10000


def decode(image, fa, glob=None):
    """Tokens, the (kind, value) fields of each instruction and, with `glob`, the instructions that access memory through
    a register loaded from the guest global `glob`."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_EBP, X86_REG_ESP
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    f = image.funcs[fa]
    o = image.offset(fa)
    tokens, fields, derived = [], [], set()
    live = {}
    for index, ins in enumerate(md.disasm(image.data[o:o + f["end"] - fa], fa)):
        values = []
        setter = None
        for op in ins.operands:
            if op.type == X86_OP_MEM:
                base, disp = op.mem.base, op.mem.disp
                if glob is not None and base == 0 and op.mem.index == 0 and (disp & 0xFFFFFFFF) == glob and \
                        ins.mnemonic == "mov" and ins.operands[0].type == X86_OP_REG:
                    setter = ins.operands[0].reg
                    continue
                if glob is not None and base in live and LOW <= abs(disp) < HIGH:
                    derived.add(index)
                if abs(disp) < 0x10000 and disp and not (base == X86_REG_ESP or (base == X86_REG_EBP and disp < 0)):
                    values.append(("d", disp))
            elif op.type == X86_OP_IMM and 0 < (op.imm & 0xFFFFFFFF) < 0x10000 and not ins.mnemonic.startswith(("j", "call")):
                values.append(("i", op.imm & 0xFFFFFFFF))
        if glob is not None:
            _, written = ins.regs_access()
            for reg in written:
                live.pop(reg, None)
            if ins.mnemonic == "mov" and len(ins.operands) == 2 and all(o2.type == X86_OP_REG for o2 in ins.operands) and \
                    ins.operands[1].reg in live:
                live[ins.operands[0].reg] = live[ins.operands[1].reg]
            if setter is not None:
                live[setter] = glob
        tokens.append(ins.mnemonic + " " + re.sub(r"0x[0-9a-f]+", "N", ins.op_str))
        fields.append(values)
    return tokens, fields, derived


def frame_size(image, fa):
    """The bytes the function's entry sequence (leading push reg / sub esp, imm) puts below its return address."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86 import X86_OP_IMM, X86_OP_REG, X86_REG_ESP
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    o = image.offset(fa)
    size = 0
    for ins in md.disasm(image.data[o:o + 32], fa):
        if ins.mnemonic == "push" and ins.operands[0].type == X86_OP_REG:
            size += 4
        elif ins.mnemonic == "sub" and ins.operands[0].type == X86_OP_REG and ins.operands[0].reg == X86_REG_ESP and                 ins.operands[1].type == X86_OP_IMM:
            size += ins.operands[1].imm
        else:
            break
    return size


def class_sets(engine, rtti):
    owners = {int(c["vtable"], 16): n for n, c in rtti["classes"].items()}
    sets = {name: {f for f in slots if f in engine.funcs} for name, (va, slots) in engine.vtables.items()}
    for fa in engine.starts:
        for _, v in engine.funcs[fa]["refs"]:
            if v in owners and owners[v] in sets:
                sets[owners[v]].add(fa)
    return sets


class_sets_cache = {}


def strides(image, lo, hi):
    """The multipliers the code applies to an index before it reads a displacement inside [lo, hi): the strides of the
    array there, as address arithmetic (lea, shl, imul, add) builds them."""
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    found = collections.Counter()
    for fa in image.starts:
        f = image.funcs[fa]
        if not any(lo <= v < hi for _, v in f["refs"]):
            continue
        o = image.offset(fa)
        linear = {}
        symbols = 0
        for ins in md.disasm(image.data[o:o + f["end"] - fa], fa):
            ops, mn = ins.operands, ins.mnemonic
            for op in ops:
                if op.type == X86_OP_MEM and lo <= (op.mem.disp & 0xFFFFFFFF) < hi:
                    for reg, scale in ((op.mem.base, 1), (op.mem.index, op.mem.scale)):
                        if reg in linear and linear[reg][1] * scale > 1:
                            found[linear[reg][1] * scale] += 1
            if mn == "lea" and ops[1].type == X86_OP_MEM:
                terms = [(r, sc) for r, sc in ((ops[1].mem.base, 1), (ops[1].mem.index, ops[1].mem.scale)) if r]
                symbol = {linear[r][0] for r, _ in terms if r in linear}
                if len(symbol) == 1 and all(r in linear for r, _ in terms):
                    linear[ops[0].reg] = (symbol.pop(), sum(linear[r][1] * sc for r, sc in terms))
                else:
                    linear.pop(ops[0].reg, None)
            elif mn == "mov" and len(ops) == 2 and ops[0].type == X86_OP_REG:
                if ops[1].type == X86_OP_REG and ops[1].reg in linear:
                    linear[ops[0].reg] = linear[ops[1].reg]
                else:
                    symbols += 1
                    linear[ops[0].reg] = (symbols, 1)
            elif mn == "shl" and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_IMM and ops[0].reg in linear:
                linear[ops[0].reg] = (linear[ops[0].reg][0], linear[ops[0].reg][1] << ops[1].imm)
            elif mn == "imul" and ops[0].type == X86_OP_REG and ops[-1].type == X86_OP_IMM and                     (ops[1] if len(ops) == 3 else ops[0]).type == X86_OP_REG and (ops[1] if len(ops) == 3 else ops[0]).reg in linear:
                source = linear[(ops[1] if len(ops) == 3 else ops[0]).reg]
                linear[ops[0].reg] = (source[0], source[1] * ops[-1].imm)
            elif mn in ("add", "sub") and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[1].type == X86_OP_REG and                     ops[0].reg in linear and ops[1].reg in linear and linear[ops[0].reg][0] == linear[ops[1].reg][0]:
                sign = 1 if mn == "add" else -1
                linear[ops[0].reg] = (linear[ops[0].reg][0], linear[ops[0].reg][1] + sign * linear[ops[1].reg][1])
            elif mn in ("add", "sub") and len(ops) == 2 and ops[1].type == X86_OP_IMM:
                continue
            else:
                _, written = ins.regs_access()
                for reg in written:
                    linear.pop(reg, None)
    return found


def named_sets(engine, config):
    out = {}
    for name, spec in config.get("sets", {}).items():
        if "functions" in spec:
            out[name] = ({int(v, 16) for v in spec["functions"] if int(v, 16) in engine.funcs}, None)
        elif "classes" in spec or "functions_and_classes" in spec:
            funcs = set().union(*(class_sets_cache[c] for c in spec.get("classes", []) if c in class_sets_cache))
            funcs |= {int(v, 16) for v in spec.get("also", []) if int(v, 16) in engine.funcs}
            out[name] = (funcs, None)
        elif "range" in spec:
            lo, hi = (int(v, 16) for v in spec["range"])
            out[name] = ({fa for fa in engine.starts if lo <= fa < hi}, None)
        elif "table" in spec:
            lo = int(spec["table"], 16)
            hi = lo + spec.get("length", 1)
            out[name] = ({fa for fa in engine.starts if any(lo <= v < hi for _, v in engine.funcs[fa]["refs"])}, None)
        elif "bytes" in spec:
            out[name] = (set(), None)
        elif "class_prefix" in spec:
            out[name] = (set().union(*(f for c, f in class_sets_cache.items() if c.startswith(spec["class_prefix"]))), None)
        elif "callers_of" in spec:
            target = int(spec["callers_of"], 16)
            out[name] = ({fa for fa in engine.starts if any(t == target for _, t in engine.funcs[fa]["calls"])}, None)
        else:
            glob = int(spec["global"], 16)
            out[name] = ({fa for fa in engine.starts if any(v == glob for _, v in engine.funcs[fa]["refs"])}, glob)
    return out


def port_sets(engine):
    """port:<file> -> the 1.0.116 functions that host source file overrides (src/host/guest_override_list.h names the file of each)."""
    out = collections.defaultdict(set)
    text = (ROOT / "src" / "host" / "guest_override_list.h").read_text(encoding="utf-8")
    for m in re.finditer(r"^GUEST_OVERRIDE\(([0-9A-F]{8}), \w+\)(.*)$", text, re.M):
        va = int(m.group(1), 16)
        for name in re.findall(r"\(([a-z_0-9]+\.c)\)", m.group(2)):
            if va in engine.funcs:
                out["port:" + name].add(va)
    return {name: (funcs, None) for name, funcs in out.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile")
    ap.add_argument("--exe", type=Path)
    args = ap.parse_args()
    engine = guest_map.load_image(ROOT / "game" / TARGET["installed_name"])
    other = guest_map.load_image(args.exe or guest_map.profile_exe(args.profile))
    mapper = guest_map.Mapper(engine, other).run()
    rtti = json.loads((ROOT / "analysis" / "rtti.json").read_text(encoding="utf-8"))
    config = json.loads(STRUCTS.read_text(encoding="utf-8"))
    class_sets_cache.update(class_sets(engine, rtti))
    entries = {name: (funcs, None) for name, funcs in class_sets_cache.items()}
    entries.update(named_sets(engine, config))
    entries.update(port_sets(engine))
    cache = {}

    def compare(fa, glob):
        key = (fa, glob)
        if key in cache:
            return cache[key]
        fb = mapper.match.get(fa)
        if fb is None:
            cache[key] = None
            return None
        ta, xa, derived = decode(engine, fa, glob)
        access = collections.Counter()
        moves = collections.defaultdict(set)
        keep = (lambda i: i in derived) if glob is not None else (lambda i: True)
        if engine.funcs[fa]["hash"] == other.funcs[fb]["hash"]:
            for i, fields in enumerate(xa):
                if keep(i):
                    for kind, value in fields:
                        if kind != "d" or LOW <= value < HIGH:
                            access[(kind, value, True)] += 1
            cache[key] = ("identical", access, moves)
            return cache[key]
        tb, xb, _ = decode(other, fb)
        matcher = difflib.SequenceMatcher(None, ta, tb, autojunk=False)
        for block in matcher.get_matching_blocks():
            for k in range(block.size):
                if not keep(block.a + k):
                    continue
                for (ka, va), (kb, vb) in zip(xa[block.a + k], xb[block.b + k]):
                    if ka == kb and (ka != "d" or LOW <= va < HIGH):
                        access[(ka, va, va == vb)] += 1
                        if va != vb:
                            moves[(ka, va)].add(vb)
        cache[key] = ("changed", access, moves)
        return cache[key]

    out = {}
    for name, (funcs, glob) in sorted(entries.items()):
        entry = {"functions": len(funcs), "identical": 0, "changed": 0, "unmatched": 0, "access": {}, "moves": {}}
        access = collections.defaultdict(lambda: [0, 0, 0])
        moves = collections.defaultdict(set)
        for fa in funcs:
            r = compare(fa, glob)
            if r is None:
                entry["unmatched"] += 1
                continue
            kind, acc, mv = r
            entry[kind] += 1
            for (ak, value, same), n in acc.items():
                access[f"{ak}:{value:#x}"][0 if kind == "identical" else (1 if same else 2)] += 1
            for (ak, value), targets in mv.items():
                moves[f"{ak}:{value:#x}"] |= targets
        entry["access"] = dict(sorted(access.items()))
        entry["moves"] = {k: [f"{t:#x}" for t in sorted(ts)] for k, ts in sorted(moves.items())}
        out[name] = entry
    for name, spec in config.get("sets", {}).items():
        if "bytes" in spec:
            base, length = int(spec["bytes"], 16), spec["length"]
            mapped = mapper.data(base)
            a, b = engine.offset(base), other.offset(mapped[0]) if mapped else None
            same = b is not None and engine.data[a:a + length] == other.data[b:b + length]
            out[name]["bytes"] = {"equal": same, "length": length, "base": f"{mapped[0]:#x}" if mapped else None}
            continue
        if "frame" in spec:
            fa = int(spec["frame"], 16)
            fb = mapper.match.get(fa)
            out[name]["frame"] = {"engine": frame_size(engine, fa), "other": frame_size(other, fb) if fb else None}
        if "table" in spec or "strides" in spec:
            lo = int((spec.get("strides") or {}).get("base") or spec["table"], 16)
            span = (spec.get("strides") or spec).get("length", 1)
            mapped_lo = mapper.data(lo)
            out[name]["strides"] = {"engine": {f"{k:#x}": n for k, n in sorted(strides(engine, lo, lo + span).items())},
                                    "other": {f"{k:#x}": n for k, n in sorted(strides(other, mapped_lo[0], mapped_lo[0] + span).items())}
                                    if mapped_lo else None}
            if spec.get("compare_bytes"):
                a, b = engine.offset(lo), other.offset(mapped_lo[0]) if mapped_lo else None
                same = b is not None and engine.data[a:a + span] == other.data[b:b + span]
                out[name]["bytes"] = {"equal": same, "length": span, "base": f"{mapped_lo[0]:#x}" if mapped_lo else None}
        if "global" not in spec and "table" not in spec:
            continue
        base = int(spec.get("global") or spec["table"], 16)
        mapped = mapper.data(base)
        absolute = {}
        for o in range(LOW, 0x400):
            vote = mapper.data(base + o)
            if vote and mapped and (not vote[2] or vote[1] >= 4 * vote[2]):
                absolute[f"{o:#x}"] = [f"{vote[0] - mapped[0]:#x}", vote[1]]
        out[name]["abs"] = {"base": [f"{mapped[0]:#x}", mapped[1]] if mapped else None, "fields": absolute}
    for name in out:
        if name not in engine.vtables:
            continue
        va, slots = engine.vtables[name]
        ova = other.vtables.get(name)
        marks = []
        for k, f in enumerate(slots):
            g = ova[1][k] if ova and k < len(ova[1]) else None
            by_code = mapper.how.get(f) in ("hash", "call")
            marks.append("y" if g is not None and mapper.match.get(f) == g and by_code else "-")
        out[name]["slots"] = "".join(marks)
    EVIDENCE.mkdir(exist_ok=True)
    (EVIDENCE / f"{args.profile}.json").write_text(json.dumps({"profile": args.profile, "classes": out}, separators=(",", ":")) + "\n",
                                                   encoding="utf-8")
    print(f"{args.profile}: {len(out)} entries ({len(config.get('sets', {}))} sets)")


if __name__ == "__main__":
    import class_layout
    class_layout.main()
