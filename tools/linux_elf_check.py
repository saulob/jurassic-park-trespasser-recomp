#!/usr/bin/env python3
"""Static checks of a Linux executable of the port. Reads the file and runs readelf and objdump; executes nothing.

  architecture    ELF64, x86-64
  glibc           the highest GLIBC_x.y symbol version the file requires against the baseline (default 2.31, Ubuntu 20.04)
  dependencies    the DT_NEEDED libraries against an allow list
  rpath           no DT_RPATH or DT_RUNPATH naming a local folder
  local paths     no home folder, drive mount, build or source folder of this machine, ASCII or UTF-16
  no Wine         no Wine, Proton or wineserver reference and no DT_NEEDED of either
  no Windows DLL  no .dll name other than the guest module names the HLE table answers for, and no DT_NEEDED .dll

  usage: tools/linux_elf_check.py ELF [--baseline 2.31] [--allow LIB ...] [--local-path TEXT ...] [--json]
Exit code 0 when every check passes, 1 when one fails, 2 when the file cannot be inspected.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ALLOWED_LIBS = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", "ld-linux-x86-64.so.2",
                "libgcc_s.so.1", "libstdc++.so.6"}
# FFmpeg's three libraries are the optional Smacker decoder: dynamic, found by the system's loader, never through an RPATH.
FFMPEG_LIB = re.compile(r"^lib(avformat|avcodec|avutil)\.so\.\d+$")
GUEST_MODULE_NAMES = {"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "ddraw.dll", "dsound.dll", "winmm.dll",
                      "msvcrt.dll", "smackw32.dll", "processor.dll", "msvcirt.dll", "lz32.dll", "comctl32.dll", "a3d.dll"}
LOCAL_PATH_PATTERNS = [rb"/home/[A-Za-z0-9_.-]+/", rb"/mnt/[a-z]/", rb"/Users/[A-Za-z0-9_.-]+/", rb"[A-Za-z]:\\\\?VisualStudio",
                       rb"S:\\\\?VisualStudio", rb"/root/"]
# What would mean the program starts or links Wine. SDL3 holds two harmless strings that name Wine to avoid its Steam
# virtual gamepad path ("wine64-preloader", "Wine launched by Steam"); they are reported, not failed.
WINE_PATTERNS = [rb"wineserver", rb"winelib", rb"libwine", rb"WINEPREFIX", rb"WINELOADER", rb"WINEDEBUG", rb"/usr/bin/wine",
                 rb"proton[-_ ]?run", rb"steam-runtime-launch", rb"STEAM_COMPAT"]
SDL_WINE_DETECTION = [rb"wine64-preloader", rb"Wine launched by Steam"]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{cmd[0]} failed: {r.stderr.strip()[:200]}")
    return r.stdout


def version_tuple(text):
    return tuple(int(x) for x in text.split("."))


def check_architecture(path):
    header = run(["readelf", "-h", str(path)])
    cls = re.search(r"Class:\s+(\S+)", header)
    machine = re.search(r"Machine:\s+(.+)", header)
    kind = re.search(r"Type:\s+(\S+)", header)
    ok = bool(cls and cls.group(1) == "ELF64" and machine and "X86-64" in machine.group(1) and kind and kind.group(1) == "DYN")
    return ok, (f"{cls.group(1) if cls else '?'} {machine.group(1).strip() if machine else '?'}, "
                f"{'position-independent (PIE)' if kind and kind.group(1) == 'DYN' else 'not PIE: guest addresses could alias code'}")


def check_glibc(path, baseline):
    needed = {}
    for line in run(["objdump", "-T", str(path)]).splitlines():
        for m in re.finditer(r"\(?GLIBC_(\d+(?:\.\d+)+)\)?\s+(\S+)", line):
            needed.setdefault(version_tuple(m.group(1)), set()).add(m.group(2))
    highest = max(needed) if needed else (0,)
    over = {".".join(map(str, v)): sorted(s)[:6] for v, s in needed.items() if v > version_tuple(baseline)}
    return not over, f"highest GLIBC_{'.'.join(map(str, highest))}", over


def check_dependencies(path, allow):
    out = run(["readelf", "-d", str(path)])
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)
    unexpected = [n for n in needed if n not in allow and not FFMPEG_LIB.match(n)]
    rpath = re.findall(r"\((?:RPATH|RUNPATH)\)\s+Library (?:rpath|runpath): \[([^\]]*)\]", out)
    return needed, unexpected, rpath


def scan_strings(data, patterns):
    hits = []
    for pattern in patterns:
        for m in re.finditer(pattern, data):
            hits.append(m.group(0).decode("latin1"))
    return sorted(set(hits))


def scan_utf16(data, texts):
    hits = []
    for text in texts:
        encoded = text.encode("utf-16-le")
        if encoded in data:
            hits.append(text)
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--baseline", default="2.31")
    ap.add_argument("--allow", nargs="*", default=[])
    ap.add_argument("--local-path", nargs="*", default=[], help="extra machine-specific text to look for")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    path = Path(args.elf)
    if not path.is_file():
        print(f"linux_elf_check: {path} not found", file=sys.stderr)
        return 2
    try:
        data = path.read_bytes()
        arch_ok, arch = check_architecture(path)
        glibc_ok, glibc, over = check_glibc(path, args.baseline)
        needed, unexpected, rpath = check_dependencies(path, ALLOWED_LIBS | set(args.allow))
    except (RuntimeError, OSError) as e:
        print(f"linux_elf_check: cannot inspect {path}: {e}", file=sys.stderr)
        return 2

    local_patterns = LOCAL_PATH_PATTERNS + [re.escape(t.encode()) for t in args.local_path]
    local_hits = scan_strings(data, local_patterns)
    local_wide = scan_utf16(data, ["/home/", "/mnt/", "VisualStudio"] + list(args.local_path))
    wine_hits = scan_strings(data, WINE_PATTERNS)
    wine_needed = [n for n in needed if re.search(r"wine|proton", n, re.I)]
    sdl_wine = [p.decode() for p in SDL_WINE_DETECTION if p in data]
    # A whole C string (a NUL on each side): the bytes of an instruction that builds a name in registers sit next to opcodes.
    dll_names = {m.group(1).decode("latin1").lower() for m in re.finditer(rb"\x00([A-Za-z0-9_+-]{2,32}\.(?:dll|DLL))\x00", data)}
    # A GUID's last byte (0x46, "F") can sit right in front of a merged module-name string: that is data, not another name.
    stray_dlls = sorted(n for n in dll_names - GUEST_MODULE_NAMES if n[1:] not in GUEST_MODULE_NAMES)
    dll_needed = [n for n in needed if n.lower().endswith(".dll")]

    rows = [
        ("architecture", arch_ok, arch),
        (f"glibc <= {args.baseline}", glibc_ok, glibc + ("; over the baseline: " + json.dumps(over) if over else "")),
        ("dependencies", not unexpected, ", ".join(needed) + (f"; unexpected: {unexpected}" if unexpected else "")),
        ("rpath/runpath", not rpath, "none" if not rpath else f"set: {rpath}"),
        ("local paths", not local_hits and not local_wide, "none" if not (local_hits or local_wide) else f"found: {(local_hits + local_wide)[:5]}"),
        ("no Wine/Proton", not wine_hits and not wine_needed,
         ("none" if not (wine_hits or wine_needed) else f"found: {(wine_hits + wine_needed)[:5]}")
         + (f"; SDL3's own Wine detection strings: {sdl_wine}" if sdl_wine else "")),
        ("no Windows DLL", not stray_dlls and not dll_needed,
         f"guest module names in the HLE table only ({len(dll_names & GUEST_MODULE_NAMES)})" if not (stray_dlls or dll_needed) else f"found: {stray_dlls + dll_needed}"),
    ]
    if args.json:
        json.dump([{"check": n, "ok": ok, "detail": d} for n, ok, d in rows], sys.stdout, indent=1)
    else:
        print(f"linux_elf_check: {path}")
        for name, ok, detail in rows:
            print(f"  {'PASS' if ok else 'FAIL'}  {name:<18} {detail}")
    return 0 if all(ok for _, ok, _ in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
