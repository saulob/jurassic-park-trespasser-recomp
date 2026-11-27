#!/usr/bin/env python3
"""The Linux half of the gate: runs the headless suites already built, the Python suites and tools the Windows gate also
runs, and the static checks of the Linux executable, and reports each as one row.

    python3 -I tools/linux_gate.py --build-dir DIR [--machine] [--filter TEXT] [--list] [--elf-baseline 2.31]

Headless: SDL's dummy audio and video drivers, no window, no sound, no launcher. The executable is inspected and asked only for
--build-info and --self-check, which open nothing.
With --machine every row is a line for tools/test.ps1, which prints them in its own format:

    @@INFO|key|value
    @@PLAN|rows
    @@SUITE|name|PASS|FAIL|BLOCKED|checks|seconds|unavailable 0 or 1|summary
    @@NOTE|name|text
    @@OUT|name|a line of the output of a failing row
    @@END|passed|failed|blocked|seconds

A suite that prints DATA UNAVAILABLE and ran at most two checks is "unavailable": it ran nothing it could prove, so the row is
BLOCKED for every feature that needs it, never a pass. Exit code 0 when no row failed."""
import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import linux_artifact  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
CHECKS = re.compile(r"(\d+) checks")
PASSED_ZERO = re.compile(r"tests passed: 0 failure|\d+ passed, 0 failed", re.I)


class Row:
    def __init__(self, name, status, checks=0, seconds=0.0, summary="", unavailable=False, notes=None, output=None):
        self.name, self.status, self.checks, self.seconds = name, status, checks, seconds
        self.summary, self.unavailable = summary, unavailable
        self.notes, self.output = notes or [], output or []


def windows_name(text):
    return text.replace("/", "\\")


def run(command, cwd, env, timeout):
    started = time.monotonic()
    try:
        done = subprocess.run(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        text = done.stdout.decode("utf-8", "replace")
        return done.returncode, text, time.monotonic() - started
    except subprocess.TimeoutExpired as error:
        text = (error.stdout or b"").decode("utf-8", "replace") + "\nTIMEOUT: stopped after %d s" % timeout
        return 124, text, time.monotonic() - started


def notes_of(text):
    return [line.strip() for line in text.splitlines() if re.search(r"DATA UNAVAILABLE|\bSKIP\b|skipped", line)][:4]


def exe_row(build_dir, name, env):
    exe = build_dir / name
    if not exe.is_file():
        return Row(name, "FAIL", summary="not built", output=["%s is not in %s" % (name, build_dir)])
    code, text, seconds = run([str(exe), "game"], REPO, env, 900)
    counts = [int(m.group(1)) for m in CHECKS.finditer(text)]
    passed = [int(m.group(1)) for m in re.finditer(r"(\d+) passed, 0 failed", text)]
    checks = counts[-1] if counts else (passed[-1] if passed else (1 if PASSED_ZERO.search(text) else 0))
    summary = next((line.strip() for line in reversed(text.splitlines()) if CHECKS.search(line) or PASSED_ZERO.search(line)), "")
    notes = notes_of(text)
    unavailable = "DATA UNAVAILABLE" in text and checks <= 2
    status = "PASS" if code == 0 and summary else "FAIL"
    output = [] if status == "PASS" else text.splitlines()[-25:]
    return Row(name, status, checks, seconds, summary[:110], unavailable, notes, output)


def python_row(name, env):
    command = [sys.executable, "-I", "-B", "-m", "unittest", "discover", "-s", "tests", "-p", name, "-v"]
    code, text, seconds = run(command, REPO, env, 1200)
    ran = re.search(r"Ran (\d+) tests?", text)
    checks = int(ran.group(1)) if ran else 0
    ok = code == 0 and re.search(r"^OK", text, re.M) is not None
    skipped = re.search(r"skipped=(\d+)", text)
    notes = ["skipped %s" % skipped.group(1)] if skipped else []
    return Row(windows_name("tests/" + name), "PASS" if ok else "FAIL", checks, seconds, "%d tests" % checks, False, notes, [] if ok else text.splitlines()[-25:])


def tool_row(spec, env):
    parts = spec.split()
    command = [sys.executable, "-B", "-X", "utf8"] + [str(REPO / parts[0])] + parts[1:]
    code, text, seconds = run(command, REPO, env, 900)
    last = next((line.strip() for line in reversed(text.splitlines()) if line.strip()), "")
    return Row(windows_name(spec), "PASS" if code == 0 else "FAIL", 1, seconds, last[:110], False, [], [] if code == 0 else text.splitlines()[-25:])


def elf_rows(build_dir, baseline):
    elf = build_dir / "bin" / "trespasser_recomp"
    if not elf.is_file():
        return [Row("linux executable", "FAIL", summary="not built", output=["%s is not built" % elf])], None
    command = [sys.executable, "-I", str(REPO / "tools" / "linux_elf_check.py"), str(elf), "--baseline", baseline, "--local-path", str(REPO), os.path.expanduser("~")]
    code, text, seconds = run(command, REPO, os.environ.copy(), 300)
    rows, highest = [], None
    for line in text.splitlines():
        m = re.match(r"\s+(PASS|FAIL)\s+(.+?)\s{2,}(.*)$", line)
        if not m:
            continue
        name = "elf " + m.group(2).strip()
        rows.append(Row(name, m.group(1), 1, 0.0, m.group(3).strip()[:110], False, [], [line] if m.group(1) == "FAIL" else []))
        h = re.search(r"highest GLIBC_(\d+\.\d+)", m.group(3))
        if h:
            highest = h.group(1)
    if not rows:
        rows.append(Row("elf checks", "FAIL", summary="no result", output=text.splitlines()[-20:]))
    return rows, highest


def artifact_rows(build_dir):
    elf = build_dir / "bin" / "trespasser_recomp"
    if not elf.is_file():
        return [Row("elf self-check", "FAIL", summary="not built")]
    started = time.monotonic()
    results = linux_artifact.check_elf(elf, REPO)
    seconds = (time.monotonic() - started) / max(1, len(results))
    return [Row(r.name, "PASS" if r.ok else "FAIL", 1, seconds, r.summary[:110], False, [], r.lines) for r in results]


def link_row(build_dir):
    lifted, core, guest = build_dir / "libjpt_lifted_linux.a", build_dir / "libjpt_linux_core.a", build_dir / "libjpt_linux_guest.a"
    if not lifted.is_file():
        return Row("lifted guest link", "FAIL", summary="libjpt_lifted_linux.a is not built", output=["configure with JPT_LINUX_LIFTED=ON"])
    command = [sys.executable, "-I", str(REPO / "tools" / "linux_lifted_link_report.py"), str(lifted), "--provided-by", str(core), str(guest)]
    code, text, seconds = run(command, REPO, os.environ.copy(), 600)
    last = next((line.strip() for line in text.splitlines() if "still needed" in line), "")
    return Row("lifted guest link", "PASS" if code == 0 else "FAIL", 1, seconds, last[:110], False, [], [] if code == 0 else text.splitlines()[-20:])


def plan(config, build_dir, flt):
    rows = []
    for name in config["portable"] + config["linux_specific"]:
        rows.append(("exe", name))
    for name in config["python"]:
        rows.append(("python", name))
    for spec in config["tools"]:
        rows.append(("tool", spec))
    rows += [("elf", ""), ("link", "")]
    if flt:
        low = flt.lower()
        rows = [r for r in rows if low in r[1].lower() or low in r[0]]
    return rows


class Reporter:
    def __init__(self, machine):
        self.machine, self.passed, self.failed, self.blocked = machine, 0, 0, 0

    def info(self, key, value):
        if self.machine:
            print("@@INFO|%s|%s" % (key, value), flush=True)
        else:
            print("  %s: %s" % (key, value), flush=True)

    def row(self, r):
        if r.status == "PASS":
            self.passed += 1
        elif r.status == "FAIL":
            self.failed += 1
        else:
            self.blocked += 1
        if self.machine:
            print("@@SUITE|%s|%s|%d|%.1f|%d|%s" % (r.name, r.status, r.checks, r.seconds, 1 if r.unavailable else 0, r.summary), flush=True)
            for note in r.notes:
                print("@@NOTE|%s|%s" % (r.name, note), flush=True)
            for line in r.output:
                print("@@OUT|%s|%s" % (r.name, line.rstrip()), flush=True)
        else:
            print("%-8s %s  %s%s" % (r.status, r.name, r.summary, "  [DATA UNAVAILABLE]" if r.unavailable else ""), flush=True)
            for line in r.output:
                print("           " + line.rstrip(), flush=True)
            for note in r.notes:
                print("           " + note, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--machine", action="store_true")
    parser.add_argument("--filter", default="")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--elf-baseline", default="2.31")
    parser.add_argument("--jobs", type=int, default=0, help="Python workers beside the suite programs; 0 (the default) runs one process at a time")
    parser.add_argument("--sanitize", action="store_true", help="only the suites of an AddressSanitizer + UndefinedBehaviorSanitizer build")
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    config = json.loads((REPO / "tests" / "linux_suites.json").read_text(encoding="utf-8"))
    rows = plan(config, build_dir, args.filter)
    if args.sanitize:
        rows = [r for r in rows if r[0] == "exe" and r[1] not in config.get("sanitizer_exclusions", [])]
    if args.list:
        for kind, name in rows:
            print("%-7s %s" % (kind, name))
        return 0

    env = os.environ.copy()
    env.update({"SDL_AUDIODRIVER": "dummy", "SDL_VIDEODRIVER": "dummy", "PYTHONDONTWRITEBYTECODE": "1", "JPT_AUDIO_HEADLESS": "1"})
    reporter = Reporter(args.machine)
    started = time.monotonic()
    reporter.info("glibc", subprocess.run(["ldd", "--version"], stdout=subprocess.PIPE).stdout.decode().splitlines()[0].split()[-1])
    reporter.info("python", "%d.%d.%d" % sys.version_info[:3])
    if args.machine:
        print("@@PLAN|%d" % (len(rows) + (1 if any(k == "elf" for k, _ in rows) else 0) * 9), flush=True)

    background = [(kind, name) for kind, name in rows if kind in ("python", "tool")]
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) if args.jobs > 0 else None
    futures = {}
    for kind, name in background if pool else []:
        futures[(kind, name)] = pool.submit(python_row if kind == "python" else tool_row, name, env)

    highest = None
    for kind, name in rows:
        if kind == "exe":
            reporter.row(exe_row(build_dir, name, env))
    for kind, name in rows:
        if kind in ("python", "tool"):
            reporter.row(futures[(kind, name)].result() if pool else (python_row if kind == "python" else tool_row)(name, env))
        elif kind == "elf":
            elf, highest = elf_rows(build_dir, args.elf_baseline)
            for r in elf + artifact_rows(build_dir):
                reporter.row(r)
        elif kind == "link":
            reporter.row(link_row(build_dir))
    if pool:
        pool.shutdown()
    if highest:
        reporter.info("glibc_max", highest)
    seconds = time.monotonic() - started
    if args.machine:
        print("@@END|%d|%d|%d|%.1f" % (reporter.passed, reporter.failed, reporter.blocked, seconds), flush=True)
    else:
        print("== result: %d passed, %d failed, %d blocked, %.0f s" % (reporter.passed, reporter.failed, reporter.blocked, seconds))
    return 1 if reporter.failed else 0


if __name__ == "__main__":
    sys.exit(main())
