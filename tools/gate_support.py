"""Readers of the port's own source tables, for the contract suites of tools\\test.ps1 (headless, read only).

Everything is derived from the sources the game is built from, so a build, language, cheat or setting added there is
seen by the gate without a list kept here. The suites report their results to the gate through report().
"""
import json
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
COMMON = SRC / "common"
HOST = SRC / "host"
LAUNCHER = ROOT / "launcher"
TESTS = ROOT / "tests"
CONFIG = ROOT / "config"

STRING = r'"(?:[^"\\]|\\.)*"'
STRINGS = rf"{STRING}(?:\s*{STRING})*"


def read(path):
    return Path(path).read_text(encoding="utf-8", errors="replace")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def c_unescape(text):
    out, i = bytearray(), 0
    simple = {"n": 10, "t": 9, "r": 13, "0": 0}
    while i < len(text):
        if text[i] != "\\":
            out += text[i].encode("utf-8")
            i += 1
        elif text[i + 1] == "x":
            j = i + 2
            while j < len(text) and j < i + 4 and text[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(text[i + 2:j], 16))
            i = j
        else:
            out.append(simple.get(text[i + 1], ord(text[i + 1]) if ord(text[i + 1]) < 128 else 63))
            i += 2
    return out.decode("utf-8", "replace")


def string_value(token):
    return "".join(c_unescape(p[1:-1]) for p in re.findall(STRING, token))


def split_top(text, sep=","):
    """Splits at the separators outside quotes and brackets."""
    out, depth, quote, start, i = [], 0, None, 0, 0
    while i < len(text):
        ch = text[i]
        if quote:
            if ch == "\\":
                i += 1
            elif ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
        elif ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth -= 1
        elif ch == sep and depth == 0:
            out.append(text[start:i])
            start = i + 1
        i += 1
    out.append(text[start:])
    return [part.strip() for part in out]


def balanced(text, open_at):
    """The text between the bracket at open_at and its match."""
    pair = {"(": ")", "{": "}", "[": "]"}[text[open_at]]
    depth, quote, i = 0, None, open_at
    while i < len(text):
        ch = text[i]
        if quote:
            if ch == "\\":
                i += 1
            elif ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
        elif ch == text[open_at]:
            depth += 1
        elif ch == pair:
            depth -= 1
            if depth == 0:
                return text[open_at + 1:i]
        i += 1
    raise ValueError("unbalanced bracket")


def array_body(text, name):
    match = re.search(rf"\b{re.escape(name)}\s*(?:\[[^\]]*\])+\s*=\s*\{{", text)
    if not match:
        raise KeyError(name)
    return balanced(text, match.end() - 1)


def rows(body):
    """The `{ ... }` rows of an array initializer, each split into its fields."""
    out, i = [], 0
    while i < len(body):
        if body[i] == "{":
            inner = balanced(body, i)
            out.append(split_top(strip_comments(inner)))
            i += len(inner) + 2
        else:
            i += 1
    return out


def text_of(token):
    if token == "NULL":
        return None
    return string_value(token) if token.startswith('"') else token


def define(text, name):
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(\S+)", text, re.M)
    return match.group(1) if match else None


def game_versions():
    """The supported builds, as the table of jpt_game_version.c lists them."""
    source = strip_comments(read(COMMON / "jpt_game_version.c"))
    names = ("id", "label", "language", "language_code", "release", "build", "lift", "text_strings", "text_codepage",
             "font_file", "font_face", "saves", "unplayable")
    out = []
    for row in rows(array_body(source, "jpt_game_versions")):
        entry = dict(zip(names, (text_of(f) for f in row)))
        entry["build"], entry["text_codepage"] = int(entry["build"]), int(entry["text_codepage"])
        out.append(entry)
    count = int(define(read(COMMON / "jpt_game_version.h"), "JPT_GAME_VERSION_COUNT"))
    if count != len(out):
        raise AssertionError(f"JPT_GAME_VERSION_COUNT is {count}, the table lists {len(out)}")
    return out


def languages():
    source = strip_comments(read(COMMON / "jpt_l10n.c"))
    out = [{"code": text_of(r[0]), "name": text_of(r[1]), "codepage": int(r[2])}
           for r in rows(array_body(source, "jpt_l10n_languages"))]
    count = int(define(read(COMMON / "jpt_l10n.h"), "JPT_L10N_LANGUAGE_COUNT"))
    if count != len(out):
        raise AssertionError(f"JPT_L10N_LANGUAGE_COUNT is {count}, the table lists {len(out)}")
    return out


def credits():
    source = strip_comments(read(COMMON / "jpt_l10n.c"))
    return [{"language": text_of(r[0]), "content": text_of(r[1]), "source": text_of(r[4])}
            for r in rows(array_body(source, "jpt_l10n_credits"))]


def cheats():
    """The cheats in the order F10 lists them, with their table row."""
    header = strip_comments(read(COMMON / "jpt_cheats.h"))
    enum = re.search(r"typedef enum \{(.*?)JPT_CHEAT_COUNT\s*\}\s*jpt_cheat_t", header, re.S).group(1)
    ids = re.findall(r"JPT_CHEAT_[A-Z_0-9]+", enum)
    source = strip_comments(read(COMMON / "jpt_cheats.c"))
    names = ("id", "kind", "key_macro", "label", "help", "default", "choices", "choice_count")
    out = []
    keys = dict(re.findall(r'#define\s+(JPT_KEY_CHEAT_[A-Z_]+)\s+("[^"]+")', header))
    for row in rows(array_body(source, "jpt_cheats")):
        entry = dict(zip(names, row))
        entry["label"] = text_of(entry["label"])
        entry["help"] = text_of(entry["help"])
        macro = entry["key_macro"]
        entry["key"] = None if macro == "NULL" else text_of(keys.get(macro, macro))
        out.append(entry)
    if [e["id"] for e in out] != ids:
        raise AssertionError("jpt_cheats[] is not in the order of the jpt_cheat_t enumeration")
    return out


def settings():
    """(key constant or literal, default text) of jpt_settings[]."""
    source = strip_comments(read(COMMON / "jpt_config.c"))
    return [(r[0], text_of(r[1])) for r in rows(array_body(source, "jpt_settings"))]


def key_constants():
    values = {}
    for header in COMMON.glob("*.h"):
        for name, token in re.findall(rf"^#define\s+(JPT_KEY_\w+)\s+({STRINGS})", read(header), re.M):
            values[name] = string_value(token)
    return values


def setting_keys():
    """{setting name: default text} with the constants resolved."""
    constants = key_constants()
    defaults = {"JPT_L10N_ENGLISH": "en"}
    out = {}
    for key, default in settings():
        name = constants.get(key, text_of(key))
        out[name] = defaults.get(default, default)
    return out


def apply_fields():
    """{setting name: the jpt_config_t field its parser fills}, from jpt_config_apply's own statements."""
    constants = key_constants()
    source = strip_comments(read(COMMON / "jpt_config.c"))
    start = source.index("static int apply(")
    body = source[start:source.index("int jpt_config_apply(")]
    out = {}
    for chunk in re.split(r"(?=if \(!strcmp\(key, )", body):
        match = re.match(r"if \(!strcmp\(key, (\w+)\)\)", chunk)
        if not match:
            continue
        field = re.search(r"c->(\w+)", chunk)
        if field:
            out[constants.get(match.group(1), match.group(1))] = field.group(1)
    return out


def host_sources():
    return [p for p in list(HOST.rglob("*.c")) + list(HOST.rglob("*.cpp")) + list(HOST.rglob("*.h"))]


def report(name, data):
    """The gate's report folder (JPT_GATE_REPORT): one JSON file per suite, read by tools\\test.ps1 for its summaries."""
    folder = os.environ.get("JPT_GATE_REPORT")
    if not folder:
        return
    Path(folder).mkdir(parents=True, exist_ok=True)
    Path(folder, f"{name}.json").write_text(json.dumps(data, indent=1), encoding="utf-8")
