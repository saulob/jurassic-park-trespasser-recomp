#!/usr/bin/env python3
"""Structure-layout differences between 1.0.116 and another build, found in the aligned functions.

    py -3 tools/layout_diff.py <profile id> [--exe <trespass.exe>] [--top N]

Instructions of matched functions are aligned with their memory displacements (and immediates) below 0x10000 masked.
Where the aligned instructions then differ only in such a displacement, a field moved: (116 offset, other offset) pairs
are counted per function and overall. A pair that many functions agree on is a real field move; one that one function
shows may be an unrelated constant. Read only.
"""
import argparse
import collections
import difflib
import json
import re
import struct
import sys
from pathlib import Path

from common import ROOT, TARGET
import guest_map

SMALL = 0x10000


def operands(ins):
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_EBP, X86_REG_ESP
    for op in ins.operands:
        if op.type == X86_OP_MEM and abs(op.mem.disp) < SMALL and op.mem.disp:
            local = op.mem.base == X86_REG_ESP or (op.mem.base == X86_REG_EBP and op.mem.disp < 0)
            if not local:
                yield "d", op.mem.disp
        elif op.type == X86_OP_IMM and 0 < (op.imm & 0xFFFFFFFF) < SMALL and not ins.mnemonic.startswith(("j", "call")):
            yield "i", op.imm & 0xFFFFFFFF


def decode(image, fa):
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    f = image.funcs[fa]
    o = image.offset(fa)
    tokens, fields = [], []
    for ins in md.disasm(image.data[o:o + f["end"] - fa], fa):
        values = list(operands(ins))
        text = ins.mnemonic + " " + re.sub(r"0x[0-9a-f]+", "N", ins.op_str)
        tokens.append(text)
        fields.append(values)
    return tokens, fields


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile")
    ap.add_argument("--exe", type=Path)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--all", action="store_true", help="also compare matches that are only candidates")
    args = ap.parse_args()
    engine = guest_map.load_image(ROOT / "game" / TARGET["installed_name"])
    other = guest_map.load_image(args.exe or guest_map.profile_exe(args.profile))
    mapper = guest_map.Mapper(engine, other).run()
    pair_functions = collections.defaultdict(set)
    total = changed_functions = compared = 0
    for fa, fb in mapper.match.items():
        if (not args.all and not mapper.proven(fa)) or engine.funcs[fa]["hash"] == other.funcs[fb]["hash"]:
            continue
        compared += 1
        ta, xa = decode(engine, fa)
        tb, xb = decode(other, fb)
        sm = difflib.SequenceMatcher(None, ta, tb, autojunk=False)
        moved = False
        for m in sm.get_matching_blocks():
            for k in range(m.size):
                for (ka, va), (kb, vb) in zip(xa[m.a + k], xb[m.b + k]):
                    if ka == kb and va != vb:
                        pair_functions[(ka, va, vb)].add(fa)
                        moved = True
        changed_functions += moved
        total += 1
    print(f"{args.profile}: {compared} proven changed functions compared, {changed_functions} show a moved field")
    ranked = sorted(pair_functions.items(), key=lambda kv: -len(kv[1]))
    for (kind, va, vb), funcs in ranked[:args.top]:
        print(f"  {'+' if vb > va else ''}{vb - va:#x}  0x{va:X} -> 0x{vb:X}  ({'displacement' if kind == 'd' else 'immediate'})"
              f"  in {len(funcs)} functions")
    shifts = collections.Counter(vb - va for (kind, va, vb), funcs in pair_functions.items() if kind == "d")
    print("  most common displacement shifts:", ", ".join(f"{d:+#x} x{n}" for d, n in shifts.most_common(8)))
    for shift, n in shifts.most_common(4):
        olds = sorted(va for (kind, va, vb), f in pair_functions.items() if kind == "d" and vb - va == shift)
        funcs = set().union(*(f for (kind, va, vb), f in pair_functions.items() if kind == "d" and vb - va == shift))
        print(f"  shift {shift:+#x}: {n} offsets from 0x{olds[0]:X} to 0x{olds[-1]:X}, in {len(funcs)} functions")


if __name__ == "__main__":
    import layout_diff
    layout_diff.main()
