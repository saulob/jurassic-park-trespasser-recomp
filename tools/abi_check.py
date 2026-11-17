#!/usr/bin/env python3
"""The calling convention of the guest functions src/host calls or replaces, in another build.

    py -3 tools/abi_check.py <profile id> [--exe <trespass.exe>]

A function whose body is identical (addresses masked) has the same convention by construction. For the others the
signature is compared: the stack bytes each `ret` releases, and whether ecx / edx are read before they are written
(a this pointer or a fastcall argument). Functions of src/host that are in the guest map as a function start are
checked; a mismatch is printed and exits 1. Read only.
"""
import argparse
import json
import sys
from pathlib import Path

from common import CONFIG_DIR, GUEST_MAPS_DIR, ROOT, TARGET, sha256_of
import guest_map


def signature(image, fa):
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from capstone.x86 import X86_OP_IMM, X86_OP_REG, X86_REG_ECX, X86_REG_EDX
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    o = image.offset(fa)
    rets, written, read_first = set(), set(), set()
    furthest = fa
    for ins in md.disasm(image.data[o:o + 0x4000], fa):
        if ins.mnemonic.startswith("j") and ins.mnemonic != "jmp" and ins.operands and ins.operands[0].type == X86_OP_IMM:
            target = ins.operands[0].imm & 0xFFFFFFFF
            if fa <= target < fa + 0x4000:
                furthest = max(furthest, target)
        if ins.mnemonic == "ret":
            rets.add(ins.operands[0].imm if ins.operands else 0)
        regs_read, regs_written = ins.regs_access()
        for reg in (X86_REG_ECX, X86_REG_EDX):
            if reg in regs_read and reg not in written:
                read_first.add(reg)
        written.update(regs_written)
        if ins.mnemonic in ("xor", "sub") and len(ins.operands) == 2 and ins.operands[0].type == X86_OP_REG and \
                ins.operands[0].reg == ins.operands[1].reg:
            read_first.discard(ins.operands[0].reg)
        if ins.mnemonic in ("ret", "jmp", "int3") and ins.address + ins.size > furthest:
            break
    return sorted(rets), sorted(read_first)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile")
    ap.add_argument("--exe", type=Path)
    ap.add_argument("--write", action="store_true", help="store config/abi_results/<id>.json")
    args = ap.parse_args()
    engine = guest_map.load_image(ROOT / "game" / TARGET["installed_name"])
    other = guest_map.load_image(args.exe or guest_map.profile_exe(args.profile))
    stored = json.loads((GUEST_MAPS_DIR / f"{args.profile}.json").read_text(encoding="utf-8"))["addresses"]
    decided = {(d["profile"], d["va"]): d for d in
               json.loads((CONFIG_DIR / "abi_decisions.json").read_text(encoding="utf-8"))["decisions"]}
    identical = compared = accepted = 0
    bad = []
    for key, r in sorted(stored.items()):
        va = int(key, 16)
        if "to" not in r or va not in engine.funcs:
            continue
        to = int(r["to"], 16)
        if to not in other.funcs:
            continue
        if engine.funcs[va]["hash"] == other.funcs[to]["hash"]:
            identical += 1
            continue
        compared += 1
        a, b = signature(engine, va), signature(other, to)
        if a != b and (args.profile, key) in decided:
            accepted += 1
        elif a != b:
            bad.append((key, r["to"], a, b, r["where"]))
    print(f"{args.profile}: {identical} host-used functions identical, {compared} changed and compared "
          f"({accepted} by recorded decision), {len(bad)} mismatched")
    if args.write:
        out = CONFIG_DIR / "abi_results"
        out.mkdir(exist_ok=True)
        result = {"profile": args.profile, "sha256": sha256_of(args.exe or guest_map.profile_exe(args.profile)),
                  "identical": identical, "compared": compared, "decided": accepted, "mismatched": len(bad)}
        (out / f"{args.profile}.json").write_text(json.dumps(result, indent=1) + chr(10), encoding="utf-8")
    for key, to, a, b, where in bad:
        print(f"  0x{key} -> 0x{to}  116 {a}  build {b}  ({where})")
    return 1 if bad else 0


if __name__ == "__main__":
    import abi_check
    sys.exit(abi_check.main())
