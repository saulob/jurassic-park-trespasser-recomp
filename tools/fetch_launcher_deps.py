#!/usr/bin/env python3
"""Fetches the sources of config/launcher_deps.txt (SDL3, Dear ImGui, TRG Launcher, Squirrel) into external/ at their pinned commits and
applies launcher/patches/<name>/*.patch, the same result as Sync-LauncherDependencies in tools/build.ps1 (the stamp
external/<name>/.jpt_commit is the same text, so either tool leaves a tree the other accepts).

    python3 -I tools/fetch_launcher_deps.py [--check]

--check only reports what is missing or stale (exit 1) and fetches nothing. Needs git and network access when something is missing."""
import hashlib
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def deps():
    for line in (ROOT / "config" / "launcher_deps.txt").read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        path, url, commit = line.split()
        yield path, url, commit


def patches_for(path):
    folder = ROOT / "launcher" / "patches" / Path(path).name
    return sorted(folder.glob("*.patch"), key=lambda p: p.name) if folder.is_dir() else []


def fingerprint(patches):
    if not patches:
        return ""
    text = "".join(f"{p.name}:{hashlib.sha256(p.read_bytes()).hexdigest().upper()}\n" for p in patches)
    return hashlib.sha256(text.encode("utf-8")).hexdigest().upper()


def expected_stamp(commit, patches):
    print_ = fingerprint(patches)
    return f"{commit}\n{print_}" if print_ else commit


def current(path, commit, patches):
    stamp = ROOT / path / ".jpt_commit"
    return stamp.is_file() and stamp.read_text(encoding="ascii").strip() == expected_stamp(commit, patches)


def git(*args, cwd):
    subprocess.run(["git", *args], cwd=cwd, check=True)


def fetch(path, url, commit, patches):
    folder = ROOT / path
    if folder.exists():
        import shutil

        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    print(f"fetching {path} ({commit[:7]})")
    git("init", "--quiet", cwd=folder)
    git("fetch", "--quiet", "--depth", "1", url, commit, cwd=folder)
    git("-c", "advice.detachedHead=false", "checkout", "--quiet", "FETCH_HEAD", cwd=folder)
    for patch in patches:
        try:
            git("apply", str(patch), cwd=folder)
        except subprocess.CalledProcessError:
            sys.exit(f"patch {patch.name} does not apply to {path}")
    (folder / ".jpt_commit").write_text(expected_stamp(commit, patches), encoding="ascii")


def main():
    check = "--check" in sys.argv[1:]
    stale = []
    for path, url, commit in deps():
        patches = patches_for(path)
        if current(path, commit, patches):
            continue
        stale.append(path)
        if not check:
            fetch(path, url, commit, patches)
    if check:
        for path in stale:
            print(f"{path}: missing or not at the pinned commit and patch set")
        return 1 if stale else 0
    print("launcher sources: " + ("fetched " + ", ".join(stale) if stale else "current"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
