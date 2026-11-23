#!/usr/bin/env python3
"""A deterministic fingerprint of what the Linux executable is built from.

    python3 -I tools/source_fingerprint.py [--root DIR] [--manifest] [--json]

The manifest is one line per file, sorted by path: the SHA-256 of its content (text files with their line endings normalised, so a
checkout with CRLF and one with LF agree) and its path. The fingerprint is the SHA-256 of that manifest. Lifted code
(src/recomp/gen*) is identified by path and size, because tools/lifts.py verifies its content and hashing hundreds of megabytes on
every build would cost more than it proves. Build output, user data, logs, game data and the git folder never take part."""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

ROOTS = (
    "CMakeLists.txt",
    "cmake",
    "src",
    "third_party/vulkan",
    "config",
    "assets",
    "external/pcrecomp/runtime",
    "external/imgui",
    "tools/build-linux.sh",
    "tools/compile_vk_shaders.py",
    "tools/ffmpeg-bootstrap.sh",
    "tools/openssl-bootstrap.sh",
    "tools/gen_build_info.py",
    "tools/source_fingerprint.py",
)
EXCLUDED_DIRS = {"build", "user_data", "logs", "game", ".git", "__pycache__", "dist"}
EXCLUDED_FILES = {"config/local.json"}
SIZE_ONLY_PREFIXES = ("src/recomp/gen",)
TEXT_SUFFIXES = {
    ".c", ".h", ".cpp", ".hpp", ".cc", ".inc", ".cmake", ".txt", ".json", ".py", ".sh", ".md", ".in", ".ddf", ".ini",
    ".vert", ".frag", ".comp", ".glsl",
}


def files_under(root: Path):
    for entry in ROOTS:
        path = root / entry
        if path.is_file():
            yield entry
        elif path.is_dir():
            for folder, dirs, names in os.walk(path):
                dirs[:] = sorted(d for d in dirs if d not in EXCLUDED_DIRS)
                for name in sorted(names):
                    relative = (Path(folder) / name).relative_to(root).as_posix()
                    if relative not in EXCLUDED_FILES:
                        yield relative


def digest_of(path: Path, relative: str) -> str:
    if relative.startswith(SIZE_ONLY_PREFIXES):
        return "size:%d" % path.stat().st_size
    data = path.read_bytes()
    if path.suffix.lower() in TEXT_SUFFIXES:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def manifest(root: Path) -> str:
    lines = sorted({"%s  %s" % (digest_of(root / relative, relative), relative) for relative in files_under(root)}, key=lambda line: line.split("  ", 1)[1])
    return "\n".join(lines) + "\n"


def fingerprint(root: Path) -> str:
    return hashlib.sha256(manifest(root).encode("utf-8")).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    parser.add_argument("--manifest", action="store_true", help="print the manifest instead of its hash")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    root = Path(args.root)
    text = manifest(root)
    if args.manifest:
        sys.stdout.write(text)
        return 0
    value = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if args.json:
        print(json.dumps({"fingerprint": value, "files": text.count("\n")}))
    else:
        print(value)
    return 0


if __name__ == "__main__":
    sys.exit(main())
