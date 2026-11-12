#!/usr/bin/env python3
"""The lifted functions the host may replace (src/host/guest_override_list.h).

tools/run_lift.py names each listed function's lifted body lifted_sub_<va>; the host defines sub_<va>, the stable
entry every direct call, alias entry and dispatch reaches. Checking a generated folder (no lift, nothing written):

    py -3 tools/stable_entries.py [src/recomp/gen]
"""
import re
import sys
from pathlib import Path

from common import GEN_DIR, ROOT

OVERRIDE_LIST = ROOT / "src" / "host" / "guest_override_list.h"
LISTED = re.compile(r"^GUEST_OVERRIDE\(([0-9A-F]{8}), (REQUIRED|OPTIONAL)\)", re.M)


def listed_overrides(path=OVERRIDE_LIST):
    """The listed guest addresses, in the list's order, which must be ascending without repeats."""
    vas = [int(m.group(1), 16) for m in LISTED.finditer(Path(path).read_text(encoding="utf-8"))]
    if not vas or any(a >= b for a, b in zip(vas, vas[1:])):
        sys.exit(f"{path}: the list must be non-empty, sorted by address and without repeats")
    return vas


def lifted_name(va):
    return f"lifted_sub_{va:08X}"


def rename_definition(body, va):
    """The body with its definition renamed to the lifted name; the calls inside it keep the stable entry."""
    head = re.compile(rf"^void sub_{va:08X}\(void\) \{{", re.M)
    renamed, n = head.subn(f"void {lifted_name(va)}(void) {{", body)
    if n != 1:
        sys.exit(f"sub_{va:08X}: expected one definition in its lifted body, found {n}")
    return renamed


DEFINITION = re.compile(r"^void (lifted_)?sub_([0-9A-F]{8})\(void\) \{", re.M)


def check_folder(gen_dir):
    """Problems with the listed functions in a generated folder: each needs exactly one lifted_sub_ definition and no
    sub_ definition of its own. Returns the listed addresses and the problems."""
    vas = listed_overrides()
    listed = set(vas)
    lifted, own = {}, {}
    for path in sorted(Path(gen_dir).glob("recomp_0*.c")):
        with open(path, encoding="utf-8", errors="replace") as source:
            text = source.read()
        for m in DEFINITION.finditer(text):
            va = int(m.group(2), 16)
            if va in listed:
                table = lifted if m.group(1) else own
                table[va] = table.get(va, 0) + 1
    problems = [f"sub_{va:08X}: {lifted.get(va, 0)} lifted_sub_ definition(s), {own.get(va, 0)} sub_ definition(s)"
                for va in vas if lifted.get(va, 0) != 1 or own.get(va, 0)]
    return vas, problems


def main():
    gen_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else GEN_DIR
    vas, problems = check_folder(gen_dir)
    for line in problems:
        print(line)
    print(f"{len(vas)} listed, {len(problems)} not lifted as stable entries in {gen_dir}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
