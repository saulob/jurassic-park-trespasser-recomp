#!/usr/bin/env python3
"""Host hooks inside lifted functions (src/host/lift_hook_list.h).

Each listed address gets `RECOMP_LIFT_HOOK(lift_hook_<va>);` in front of the instruction at that address, in every
lifted body that holds it. The host defines lift_hook_<va>; it runs with the guest registers, flags and memory, may
change them, and the lifted code then carries on with that instruction. Checking a generated folder (nothing written):

    py -3 tools/lift_hooks.py [src/recomp/gen]
"""
import re
import sys
from pathlib import Path

from common import GEN_DIR, ROOT

HOOK_LIST = ROOT / "src" / "host" / "lift_hook_list.h"
LISTED = re.compile(r"^LIFT_HOOK\(([0-9A-F]{8})\)", re.M)

HOOK_MACRO = ("#define RECOMP_LIFT_HOOK(fn) do { uint32_t _hook_caller = g_cur_func; RECOMP_FLAGS_OUT(); "
              "RECOMP_REGS_OUT(); fn(); RECOMP_REGS_IN(); RECOMP_FLAGS_IN(); g_cur_func = _hook_caller; } while (0)\n")


def listed_hooks(path=HOOK_LIST):
    """The listed guest addresses, which must be ascending without repeats (an empty list is allowed)."""
    path = Path(path)
    if not path.exists():
        return []
    vas = [int(m.group(1), 16) for m in LISTED.finditer(path.read_text(encoding="utf-8"))]
    if any(a >= b for a, b in zip(vas, vas[1:])):
        sys.exit(f"{path}: the list must be sorted by address and without repeats")
    return vas


def hook_name(va):
    return f"lift_hook_{va:08X}"


def hook_call(va):
    return f"RECOMP_LIFT_HOOK({hook_name(va)}); /* host hook */"


def install(lifter, vas):
    """Makes the lifter put each hook in front of its instruction. The flags the lifter tracks statically are
    forgotten there, so the instruction reads the run-time flags the hook hands back."""
    hooks = set(vas)
    lift_instruction = lifter.lift_instruction

    def hooked(insn):
        if insn.address not in hooks:
            return lift_instruction(insn)
        lifter._flag_state = None
        return [hook_call(insn.address)] + list(lift_instruction(insn))

    lifter.lift_instruction = hooked


def header_text(vas):
    return HOOK_MACRO + "".join(f"void {hook_name(va)}(void);\n" for va in vas)


def hook_counts(gen_dir, vas):
    counts = {va: 0 for va in vas}
    for path in sorted(Path(gen_dir).glob("recomp_0*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for va in vas:
            counts[va] += text.count(hook_call(va))
    return counts


def check_folder(gen_dir):
    """Problems with the listed hooks in a generated folder: each must be in at least one lifted body."""
    vas = listed_hooks()
    counts = hook_counts(gen_dir, vas)
    return vas, [f"{hook_name(va)}: not in any lifted body" for va in vas if not counts[va]]


def main():
    gen_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else GEN_DIR
    vas, problems = check_folder(gen_dir)
    for line in problems:
        print(line)
    print(f"{len(vas)} listed, {len(problems)} missing in {gen_dir}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
