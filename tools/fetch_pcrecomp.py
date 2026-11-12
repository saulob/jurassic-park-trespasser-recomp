#!/usr/bin/env python3
"""Clone pcrecomp at the pinned commit into external/pcrecomp (MIT, not vendored) and apply patches/pcrecomp in the
order of its series file. external/pcrecomp.patches records the commit and each patch's SHA-256; tools/build.ps1
fetches again when it no longer matches. Local changes in the checkout are refused unless that record says they are
this project's patches."""
import subprocess
import sys

from common import PCRECOMP_DIR, ROOT, TARGET, sha256_of

PATCH_DIR = ROOT / "patches" / "pcrecomp"
APPLIED = PCRECOMP_DIR.parent / "pcrecomp.patches"


def git(*args, cwd=None):
    subprocess.run(["git", *args], cwd=cwd, check=True)


def git_output(*args):
    return subprocess.run(["git", *args], cwd=PCRECOMP_DIR, check=True, capture_output=True, text=True).stdout


def series():
    lines = (PATCH_DIR / "series").read_text(encoding="utf-8").splitlines()
    return [PATCH_DIR / line.strip() for line in lines if line.strip() and not line.lstrip().startswith("#")]


def record(commit, patches):
    return "".join([f"{commit}\n"] + [f"{sha256_of(p)} {p.name}\n" for p in patches])


def main():
    pin = TARGET["pcrecomp"]
    if not (PCRECOMP_DIR / ".git").is_dir():
        PCRECOMP_DIR.parent.mkdir(parents=True, exist_ok=True)
        git("clone", "--no-checkout", pin["url"], str(PCRECOMP_DIR))
    known = subprocess.run(["git", "cat-file", "-e", pin["commit"] + "^{commit}"],
                           cwd=PCRECOMP_DIR, capture_output=True).returncode == 0
    if not known:
        git("fetch", "origin", pin["commit"], cwd=PCRECOMP_DIR)
    patches = series()
    wanted = record(pin["commit"], patches)
    applied = APPLIED.read_text(encoding="utf-8") if APPLIED.exists() else None
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=PCRECOMP_DIR, capture_output=True, text=True).stdout.strip()
    if applied == wanted and head == pin["commit"]:
        print(f"pcrecomp at {pin['commit']} with {len(patches)} patch(es) in {PCRECOMP_DIR}")
        return 0
    if git_output("status", "--porcelain", "--untracked-files=no").strip():
        if applied is None:
            sys.exit(f"{PCRECOMP_DIR} has local changes that are not this project's patches: commit or discard them")
        git("checkout", "--quiet", "--", ".", cwd=PCRECOMP_DIR)
    APPLIED.unlink(missing_ok=True)
    git("checkout", "--quiet", "--detach", pin["commit"], cwd=PCRECOMP_DIR)
    for patch in patches:
        git("apply", "--check", str(patch), cwd=PCRECOMP_DIR)
        git("apply", str(patch), cwd=PCRECOMP_DIR)
        print(f"applied {patch.name}")
    APPLIED.write_bytes(wanted.encode("utf-8"))
    print(f"pcrecomp at {pin['commit']} with {len(patches)} patch(es) in {PCRECOMP_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
