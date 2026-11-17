#!/usr/bin/env python3
"""Rewrites the guest addresses of src/host into GVA(0x...) (GSUB, GHOOK for sub_<va>, lift_hook_<va> names).

    py -3 tools/gva_convert.py [--check]

An address is a literal that tools/guest_map.py finds in code (not in a comment or string, not excluded by
config/guest_va_exclude.json). Already converted code is left as it is. --check lists the raw guest addresses still
in src/host and exits 1 when there are any: the host must reach every guest address through the profile's map.
"""
import re
import sys

from common import ROOT
import guest_map

SKIP_FILES = {"src/host/guest_override_list.h", "src/host/lift_hook_list.h", "src/host/guest_va.h"}
SKIP_FILES |= guest_map.COLOUR_FILES
TOKEN = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)+\'', re.S)
LITERAL = re.compile(r"\b0x([0-9A-Fa-f]{6,8})[uUlL]*\b")
IDENT = re.compile(r"\b(sub|lift_hook)_([0-9A-F]{8})\b")
CONVERTED = re.compile(r"\b(GVA|GVA_ID|GSUB|GHOOK)\(\s*$")


def convert_code(code, known, skip, rel):
    def lit(m):
        value = int(m.group(1), 16)
        if value not in known or (rel, "0x" + m.group(1).lower()) in skip:
            return m.group(0)
        if CONVERTED.search(code[max(0, m.start() - 8):m.start()]):
            return m.group(0)
        return f"GVA(0x{value:08X})"

    def ident(m):
        value = int(m.group(2), 16)
        return f"{'GSUB' if m.group(1) == 'sub' else 'GHOOK'}(0x{value:08X})"

    code = LITERAL.sub(lit, code)
    return IDENT.sub(ident, code)


def convert(text, known, skip, rel):
    out, last = [], 0
    for m in TOKEN.finditer(text):
        out.append(convert_code(text[last:m.start()], known, skip, rel))
        out.append(m.group(0))
        last = m.end()
    out.append(convert_code(text[last:], known, skip, rel))
    return "".join(out)


def ensure_include(text):
    if not re.search(r"(GVA|GVA_ID|GSUB|GHOOK)\(", text) or '#include "guest_va.h"' in text:
        return text
    m = re.search(r'^#include ', text, re.M)
    line = '#include "guest_va.h"\n'
    return text[:m.start()] + line + text[m.start():] if m else line + text


def sources():
    for path in sorted((ROOT / "src" / "host").rglob("*")):
        rel = path.relative_to(ROOT).as_posix()
        if path.suffix in (".c", ".h", ".cpp") and rel not in SKIP_FILES and "/profiles/" not in rel:
            yield path, rel


def raw_addresses(known, skip):
    found = []
    for path, rel in sources():
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in TOKEN.finditer(text):
            text = text[:m.start()] + " " * (m.end() - m.start()) + text[m.end():]
        for n, line in enumerate(text.splitlines(), 1):
            for m in LITERAL.finditer(line):
                value = int(m.group(1), 16)
                inside = re.search(r"\b(GVA|GVA_ID|GSUB|GHOOK)\(\s*$", line[:m.start()])
                if value in known and not inside and (rel, "0x" + m.group(1).lower()) not in skip:
                    found.append(f"{rel}:{n} 0x{m.group(1)}")
            for m in IDENT.finditer(line):
                found.append(f"{rel}:{n} {m.group(0)}")
    return found


def main():
    known = set(guest_map.host_addresses())
    skip = guest_map.excluded_literals()
    if "--check" in sys.argv:
        left = raw_addresses(known, skip)
        for line in left:
            print("raw guest address:", line)
        print(f"{len(left)} raw guest addresses in src/host")
        return 1 if left else 0
    changed = 0
    for path, rel in sources():
        text = path.read_bytes().decode("utf-8", errors="replace")
        crlf = "\r\n" in text
        new = ensure_include(convert(text.replace("\r\n", "\n"), known, skip, rel))
        new = new.replace("\n", "\r\n") if crlf else new
        if new != text:
            path.write_bytes(new.encode("utf-8"))
            changed += 1
    print(f"{changed} files rewritten")
    return 0


if __name__ == "__main__":
    sys.exit(main())
