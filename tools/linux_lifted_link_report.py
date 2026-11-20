#!/usr/bin/env python3
"""What the lifted guest still needs from the host before an executable can link it.

Reads the archive of the lifted guest (libjpt_lifted_linux.a), takes every symbol it uses and does not define, and sorts
them: the runtime state and functions the lifted code expects (registers, the x87 stack, dispatch), the stable entries
of overridden functions, the hooks the host places inside lifted functions, and the C library. Executes nothing.
  usage: tools/linux_lifted_link_report.py ARCHIVE [--json]
"""
import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict


def classify(name):
    if re.match(r"^lift_hook_[0-9A-Fa-f]{8}$", name):
        return "host hook inside a lifted function"
    if re.match(r"^lift\w*_sub_[0-9A-Fa-f]{8}$", name):
        return "stable entry of an overridden function"
    if name.startswith(("g_", "recomp_")):
        return "recomp runtime (registers, x87 ring, dispatch)"
    return "C library"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("--provided-by", nargs="*", default=[], help="archives of the Linux runtime that define symbols for it")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    out = subprocess.run(["nm", "-A", "--defined-only", args.archive], capture_output=True, text=True, check=True).stdout
    defined = {line.split()[-1] for line in out.splitlines() if len(line.split()) >= 3}
    out = subprocess.run(["nm", "-u", args.archive], capture_output=True, text=True, check=True).stdout
    undefined = {line.split()[-1] for line in out.splitlines() if line.strip().startswith("U ")}
    for extra in args.provided_by:
        provided = subprocess.run(["nm", "-A", "--defined-only", extra], capture_output=True, text=True, check=True).stdout
        defined |= {line.split()[-1] for line in provided.splitlines() if len(line.split()) >= 3}
    external = sorted(undefined - defined)
    groups = defaultdict(list)
    for name in external:
        groups[classify(name)].append(name)
    if args.json:
        json.dump(groups, sys.stdout, indent=1)
        return 0
    print(f"lifted guest: {len(external)} symbols still needed from the host" +
          (f" after {len(args.provided_by)} runtime archive(s)" if args.provided_by else ""))
    for kind in ("recomp runtime (registers, x87 ring, dispatch)", "stable entry of an overridden function",
                 "host hook inside a lifted function", "C library"):
        names = groups.get(kind, [])
        print(f"  {len(names):>4}  {kind}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
