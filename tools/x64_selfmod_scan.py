"""Inventory of trespass.exe's SelfMod section for the x64 port (docs/X64_MIGRATION.md).

Reads game/trespass.exe, src/recomp/gen/*.c and build/logs/*.log; writes nothing.

    py -3 tools/x64_selfmod_scan.py [--json out.json]
"""
import argparse
import bisect
import glob
import json
import os
import re
import struct
import sys

import capstone
import pefile
from capstone import x86

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SM_LO, SM_HI = 0x0063B000, 0x00644B75


def in_sm(v):
    return SM_LO <= v < SM_HI


def load_image(path):
    pe = pefile.PE(path)
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = {}
    for s in pe.sections:
        secs[s.Name.rstrip(b"\0").decode()] = (base + s.VirtualAddress, s.get_data())
    return pe, base, secs


def lifted_refs(gen_dir):
    """SelfMod targets of lifted calls/jumps, with the enclosing lifted function."""
    fn_re = re.compile(r"^void (sub_[0-9A-F]{8})\(void\) \{")
    ref_re = re.compile(r"RECOMP_(ICALL|ITAIL|CALL)\((?:0x)?0*([0-9A-Fa-f]{6,8})u?\);\s*/\* 0x([0-9A-F]{8}): (\w+)")
    refs = {}
    for path in sorted(glob.glob(os.path.join(gen_dir, "recomp_*.c"))):
        cur = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = fn_re.match(line)
                if m:
                    cur = m.group(1)
                    continue
                if "RECOMP_" not in line:
                    continue
                m = ref_re.search(line)
                if not m:
                    continue
                tgt = int(m.group(2), 16)
                if in_sm(tgt):
                    refs.setdefault(tgt, set()).add((cur, int(m.group(3), 16), m.group(4)))
    return refs


def runtime_entries(log_dir):
    rx = re.compile(r"first native entry at 0x([0-9A-F]{8}) from (sub_[0-9A-F]{8})")
    seen = {}
    for path in sorted(glob.glob(os.path.join(log_dir, "*.log"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = rx.search(line)
                if m:
                    seen.setdefault(int(m.group(1), 16), set()).add((os.path.basename(path), m.group(2)))
    return seen


def data_pointers(secs):
    out = []
    for name in (".rdata", ".data"):
        va0, d = secs[name]
        for off in range(0, len(d) - 3, 4):
            v = struct.unpack_from("<I", d, off)[0]
            if in_sm(v):
                out.append((name, va0 + off, v))
    return out


def analyse(sm):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    md.skipdata = True
    ins = [i for i in md.disasm(sm, SM_LO)]
    addrs = [i.address for i in ins]

    def containing(va):
        k = bisect.bisect_right(addrs, va) - 1
        return ins[k] if k >= 0 else None

    starts = sorted(a for a in range(SM_LO, SM_HI - 3, 16) if sm[a - SM_LO:a - SM_LO + 3] == b"\x55\x8b\xec")
    routines = []
    for n, lo in enumerate(starts):
        hi = starts[n + 1] if n + 1 < len(starts) else SM_HI
        body = [i for i in ins if lo <= i.address < hi and i.id != 0]
        r = {
            "entry": lo, "end": hi, "insns": len(body), "x87": 0, "fdiv": 0, "store16": 0, "store8": 0,
            "texel8": 0, "texel16": 0, "backedges": 0, "calls_out": [], "pushes_sm": [], "patches": [],
            "rets": 0, "cmov": 0,
        }
        for k, i in enumerate(body):
            m = i.mnemonic
            if m.startswith("f"):
                r["x87"] += 1
                if m.startswith("fdiv"):
                    r["fdiv"] += 1
            if m.startswith("cmov"):
                r["cmov"] += 1
            if m == "ret":
                r["rets"] += 1
            ops = i.operands
            if m == "mov" and ops and ops[0].type == x86.X86_OP_MEM:
                if ops[0].size == 2:
                    r["store16"] += 1
                elif ops[0].size == 1:
                    r["store8"] += 1
            if m == "movzx" and len(ops) == 2 and ops[1].type == x86.X86_OP_MEM and ops[1].mem.base:
                r["texel8" if ops[1].size == 1 else "texel16"] += 1
            if (m.startswith("j") or m == "loop") and ops and ops[0].type == x86.X86_OP_IMM:
                if lo <= ops[0].imm < i.address:
                    r["backedges"] += 1
            if m == "call" and ops and ops[0].type == x86.X86_OP_IMM and not in_sm(ops[0].imm):
                r["calls_out"].append((i.address, ops[0].imm))
            if m == "push" and ops and ops[0].type == x86.X86_OP_IMM and in_sm(ops[0].imm):
                r["pushes_sm"].append((i.address, ops[0].imm))
            if m == "lea" and ops[1].type == x86.X86_OP_MEM and not ops[1].mem.base and not ops[1].mem.index \
                    and in_sm(ops[1].mem.disp & 0xFFFFFFFF):
                label = ops[1].mem.disp & 0xFFFFFFFF
                reg = ops[0].reg
                for j in body[k + 1:k + 4]:
                    jo = j.operands
                    if j.mnemonic == "mov" and len(jo) == 2 and jo[0].type == x86.X86_OP_MEM and jo[0].mem.base == reg \
                            and not jo[0].mem.index:
                        site = (label + jo[0].mem.disp) & 0xFFFFFFFF
                        tgt = containing(site)
                        field = "?"
                        if tgt is not None:
                            rel = site - tgt.address
                            if tgt.disp_size == 4 and rel == tgt.disp_offset:
                                field = "disp32"
                            elif tgt.imm_size == 4 and rel == tgt.imm_offset:
                                field = "imm32"
                        placeholder = struct.unpack_from("<I", sm, site - SM_LO)[0]
                        r["patches"].append({
                            "writer": j.address, "store": f"{j.mnemonic} {j.op_str}", "site": site, "size": jo[0].size,
                            "insn": tgt.address if tgt else None,
                            "insn_text": f"{tgt.mnemonic} {tgt.op_str}" if tgt else "?",
                            "field": field, "placeholder": placeholder,
                            "trailing": bool(tgt) and tgt.address + tgt.size - 4 == site, "src": j.reg_name(jo[1].reg) if jo[1].type == x86.X86_OP_REG else "?",
                        })
                        break
        routines.append(r)

    # any other store whose base register was loaded with a SelfMod address is not covered above;
    # count stores with an absolute SelfMod displacement as a cross-check (expected 0)
    abs_writes = [i.address for i in ins if i.id and i.operands and i.operands[0].type == x86.X86_OP_MEM
                  and not i.operands[0].mem.base and not i.operands[0].mem.index
                  and in_sm(i.operands[0].mem.disp & 0xFFFFFFFF) and i.mnemonic.startswith(("mov", "add", "sub", "or", "and", "xor", "inc", "dec"))]
    return routines, abs_writes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=os.path.join(ROOT, "game", "trespass.exe"))
    ap.add_argument("--json")
    a = ap.parse_args()
    if not os.path.exists(a.exe):
        sys.exit(f"{a.exe} not found")
    pe, base, secs = load_image(a.exe)
    sm_va, sm_raw = secs["SelfMod"]
    sm = sm_raw[:SM_HI - sm_va]
    lifted = lifted_refs(os.path.join(ROOT, "src", "recomp", "gen"))
    seen = runtime_entries(os.path.join(ROOT, "build", "logs"))
    dptr = data_pointers(secs)
    routines, abs_writes = analyse(sm)

    reached_static = set(lifted) | {v for _, _, v in dptr}
    for r in routines:
        e = r["entry"]
        r["lifted_callers"] = sorted({c for c, _, _ in lifted.get(e, ())} - {None})
        r["call_sites"] = sorted({s for _, s, _ in lifted.get(e, ())})
        r["runtime"] = sorted(seen.get(e, ()))
        r["data_ptr"] = [f"{n}:0x{va:08X}" for n, va, v in dptr if v == e]
        r["static"] = e in reached_static

    pushed = {t for r in routines for _, t in r["pushes_sm"]}
    print(f"SelfMod 0x{SM_LO:08X}-0x{SM_HI:08X}: {len(routines)} routines (push ebp; mov ebp,esp at 16-byte boundaries)")
    print(f"runtime-reached entries (logs): {len(seen)}; statically referenced entries: "
          f"{len([r for r in routines if r['static']])}; atexit-pushed: {len(pushed)}")
    nsites = len({s for v in lifted.values() for _, s, _ in v})
    print(f"absolute-address stores into SelfMod: {len(abs_writes)}; lifted call/jmp sites into SelfMod: {nsites}; "
          f"patch sites: {sum(len(r['patches']) for r in routines)} "
          f"(trailing dword of their instruction, as lift32 patch_sites needs: "
          f"{sum(p['trailing'] for r in routines for p in r['patches'])})")
    nonentry = sorted((set(seen) | set(lifted)) - {r['entry'] for r in routines})
    if nonentry:
        print("targets that are not a routine start:", ", ".join(f"0x{v:08X}" for v in nonentry))
    print()
    print("| entry | size | insns | runtime | lifted callers | x87/fdiv | st16/st8 | texel ld8/16 | loops | patches | out calls |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for r in routines:
        callers = ", ".join(r["lifted_callers"][:4]) + (" ..." if len(r["lifted_callers"]) > 4 else "")
        if not callers and r["data_ptr"]:
            callers = "data " + ", ".join(r["data_ptr"])
        if not callers and r["entry"] in pushed:
            callers = "atexit (pushed by SelfMod)"
        outs = ", ".join(f"0x{t:08X}" for _, t in r["calls_out"])
        print(f"| 0x{r['entry']:08X} | {r['end'] - r['entry']} | {r['insns']} | {'yes' if r['runtime'] else ''} | {callers} | "
              f"{r['x87']}/{r['fdiv']} | {r['store16']}/{r['store8']} | {r['texel8']}/{r['texel16']} | {r['backedges']} | "
              f"{len(r['patches'])} | {outs} |")
    print()
    print("| routine | writer | store | patched insn | field | placeholder | value from |")
    print("|---|---|---|---|---|---|---|")
    for r in routines:
        for p in r["patches"]:
            print(f"| 0x{r['entry']:08X} | 0x{p['writer']:08X} | `{p['store']}` | 0x{p['insn']:08X} `{p['insn_text']}` | "
                  f"{p['field']} @0x{p['site']:08X} | 0x{p['placeholder']:08X} | {p['src']} |")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"routines": routines, "abs_writes": abs_writes, "data_ptrs": dptr},
                      f, indent=1, default=list)


if __name__ == "__main__":
    main()
