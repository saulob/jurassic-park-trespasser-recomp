#!/usr/bin/env python3
"""Which headless test programs of the Windows gate compile on Linux, and why the others do not.

Reads the test targets of CMakeLists.txt, compiles every source of each with the host compiler (syntax only, SDL3 headers
from external/SDL) and prints one row per suite: portable (all sources compile) or the first source that fails and its
first error. This is the evidence behind the suite classification in tools/test-linux.sh; it runs nothing.
  usage: tools/linux_suite_scan.py [--json] [--cc gcc]
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDES = ["src/host", "src/common", "src/host/audio", "src/host/video", "src/host/ddraw", "src/host64", "tests",
            "external/SDL/include", "external/pcrecomp/runtime/recomp32", "external/pcrecomp/runtime/win32hle"]
SKIP_TARGETS = {"trespasser_recomp", "trespasser_x64_probe", "translation_pack", "stream_fix_pack"}


def test_targets():
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    targets = {}
    for m in re.finditer(r"add_executable\((\w+?)(?:_\$\{model\})?\s+EXCLUDE_FROM_ALL\s+(.*?)\)", text, re.S):
        name, body = m.group(1), m.group(2)
        sources = [t for t in re.findall(r"[\w./$\{\}-]+\.(?:c|cpp)\b", body) if "${" not in t]
        targets[name] = sources
    return {k: v for k, v in targets.items() if k not in SKIP_TARGETS}


def compile_one(cc, source):
    is_cpp = source.endswith(".cpp")
    compiler = "g++" if is_cpp else cc
    cmd = [compiler, "-fsyntax-only", "-w", "-std=gnu++20" if is_cpp else "-std=gnu11"]
    for inc in INCLUDES:
        cmd += ["-I", str(ROOT / inc)]
    cmd.append(str(ROOT / source))
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode == 0:
        return None
    for line in r.stderr.splitlines():
        if "error" in line:
            return line.strip()[:200]
    return (r.stderr.strip().splitlines() or ["failed"])[0][:200]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--cc", default="gcc")
    args = ap.parse_args()
    cache = {}
    rows = []
    for name, sources in sorted(test_targets().items()):
        failure = None
        for source in sources:
            if not (ROOT / source).is_file():
                continue
            if source not in cache:
                cache[source] = compile_one(args.cc, source)
            if cache[source] and not failure:
                failure = {"source": source, "error": cache[source]}
        rows.append({"suite": name, "sources": len(sources), "portable": failure is None, "first_failure": failure})
    if args.json:
        json.dump(rows, sys.stdout, indent=1)
        return 0
    for r in rows:
        if r["portable"]:
            print(f"PORTABLE  {r['suite']}")
        else:
            f = r["first_failure"]
            print(f"WINDOWS   {r['suite']:<28} {f['source']}: {f['error']}")
    print(f"{sum(r['portable'] for r in rows)} of {len(rows)} suites compile on this host")
    return 0


if __name__ == "__main__":
    sys.exit(main())
