#!/usr/bin/env python3
"""Regenerate analysis/ from game/trespass.exe with the pinned pcrecomp tools.

    py -3 tools/analyze.py            # RTTI seeds, then the function catalog (about 15 minutes)
    py -3 tools/analyze.py --skip-catalog
"""
import argparse
import json
import subprocess
import sys

from common import ANALYSIS_DIR, PCRECOMP_DIR, PROFILE_ID, TARGET, require_target_exe, sha256_of

PE_TOOLS = PCRECOMP_DIR / "tools"


def run(script, *args, log=None):
    cmd = [sys.executable, str(PE_TOOLS / script), *map(str, args)]
    print("[*]", " ".join(cmd[1:]))
    if log:
        with open(log, "w") as fh:
            code = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT).returncode
    else:
        code = subprocess.run(cmd).returncode
    if code:
        sys.exit(f"{script} failed with exit code {code}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--skip-catalog", action="store_true", help="keep the existing functions.json")
    ap.add_argument("--profile", default=PROFILE_ID, help="build to analyse (config/profiles/<id>.json); default 1.0.116")
    args = ap.parse_args()

    exe = require_target_exe()
    if not PE_TOOLS.is_dir():
        sys.exit(f"missing {PCRECOMP_DIR} -- run tools/fetch_pcrecomp.py first")
    ANALYSIS_DIR.mkdir(exist_ok=True)

    fingerprint = {"file": exe.name, "size": exe.stat().st_size, "sha256": sha256_of(exe),
                   "pcrecomp_commit": TARGET["pcrecomp"]["commit"]}
    (ANALYSIS_DIR / "fingerprint.json").write_text(json.dumps(fingerprint, indent=1))

    run("pe/pe_analyze.py", exe, log=ANALYSIS_DIR / "pe.txt")
    run("pe/analyze_sections.py", exe, log=ANALYSIS_DIR / "sections.txt")
    run("pe/extract_imports.py", exe, log=ANALYSIS_DIR / "imports.txt")
    run("cpp/rtti.py", exe, "-o", ANALYSIS_DIR / "rtti.json", "--seeds", ANALYSIS_DIR / "rtti_seeds.json",
        "--names", ANALYSIS_DIR / "rtti_names.json")
    if not args.skip_catalog:
        run("disasm/disasm32.py", exe, "-o", ANALYSIS_DIR / "functions.json",
            "--seed-functions", ANALYSIS_DIR / "rtti_seeds.json", log=ANALYSIS_DIR / "disasm32.log")
    print(f"[*] analysis written to {ANALYSIS_DIR}")


if __name__ == "__main__":
    main()
