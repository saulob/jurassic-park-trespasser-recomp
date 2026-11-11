#!/usr/bin/env python3
"""Clone pcrecomp at the pinned commit into external/pcrecomp (MIT, not vendored)."""
import subprocess
import sys

from common import PCRECOMP_DIR, TARGET


def git(*args, cwd=None):
    subprocess.run(["git", *args], cwd=cwd, check=True)


def main():
    pin = TARGET["pcrecomp"]
    if not (PCRECOMP_DIR / ".git").is_dir():
        PCRECOMP_DIR.parent.mkdir(parents=True, exist_ok=True)
        git("clone", "--no-checkout", pin["url"], str(PCRECOMP_DIR))
    known = subprocess.run(["git", "cat-file", "-e", pin["commit"] + "^{commit}"],
                           cwd=PCRECOMP_DIR, capture_output=True).returncode == 0
    if not known:
        git("fetch", "origin", pin["commit"], cwd=PCRECOMP_DIR)
    git("checkout", "--quiet", "--detach", pin["commit"], cwd=PCRECOMP_DIR)
    print(f"pcrecomp at {pin['commit']} in {PCRECOMP_DIR}")


if __name__ == "__main__":
    sys.exit(main())
