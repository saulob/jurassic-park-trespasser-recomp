"""Paths and fingerprints shared by the project tools."""
import hashlib
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG_DIR = ROOT / "config"
LOCAL_CONFIG = CONFIG_DIR / "local.json"
PCRECOMP_DIR = ROOT / "external" / "pcrecomp"
GAME_DIR = ROOT / "game"
BASELINE_PROFILE = "en-116"
PROFILES_DIR = CONFIG_DIR / "profiles"
GUEST_MAPS_DIR = CONFIG_DIR / "guest_maps"


def selected_profile():
    """The profile the tools work on: --profile <id>, else JPT_PROFILE, else the 1.0.116 baseline (the lift the English
    and Russian versions share)."""
    for i, arg in enumerate(sys.argv):
        if arg == "--profile" and i + 1 < len(sys.argv):
            return sys.argv[i + 1]
        if arg.startswith("--profile="):
            return arg.split("=", 1)[1]
    return os.environ.get("JPT_PROFILE", BASELINE_PROFILE)


PROFILE_ID = selected_profile()
_baseline = json.loads((CONFIG_DIR / "target.json").read_text(encoding="utf-8"))
if PROFILE_ID in (BASELINE_PROFILE, "ru-116"):
    PROFILE = None
    TARGET = _baseline
    ANALYSIS_DIR = ROOT / "analysis"
    GEN_DIR = ROOT / "src" / "recomp" / "gen"
    LIFT_ID = "lift116"
else:
    _profile_file = PROFILES_DIR / f"{PROFILE_ID}.json"
    if not _profile_file.exists():
        sys.exit(f"unknown profile {PROFILE_ID}: {_profile_file} does not exist")
    PROFILE = json.loads(_profile_file.read_text(encoding="utf-8"))
    TARGET = dict(_baseline, **{k: PROFILE[k] for k in ("sha256", "size", "file_version", "pe_timestamp", "image_base",
                                                          "size_of_image", "entry_point", "selfmod")})
    ANALYSIS_DIR = ROOT / "analysis" / PROFILE_ID
    GEN_DIR = ROOT / "src" / "recomp" / PROFILE["gen_dir"]
    LIFT_ID = PROFILE["lift"]
if PROFILE_ID == BASELINE_PROFILE or PROFILE is None:
    GUEST_EXE = Path(os.environ.get("JPT_PROFILE_EXE") or GAME_DIR / TARGET["installed_name"])
else:
    GUEST_EXE = Path(os.environ.get("JPT_PROFILE_EXE") or GAME_DIR / "versions" / PROFILE_ID / TARGET["installed_name"])


LIFT_PREFIX = f"{LIFT_ID}_"
LIFT_META_HEADER = ROOT / "src" / "common" / "jpt_lift_meta.h"


def lift_format():
    """The generated lift's shape version, from the C header the host checks it against."""
    match = re.search(r"^#define JPT_LIFT_FORMAT (\d+)u", LIFT_META_HEADER.read_text(encoding="utf-8"), re.M)
    if not match:
        sys.exit(f"{LIFT_META_HEADER}: JPT_LIFT_FORMAT is not defined")
    return int(match.group(1))


def decoder_fixups():
    if PROFILE is not None:
        return PROFILE["decoder_fixups"]
    return json.loads((CONFIG_DIR / "decoder_fixups.json").read_text(encoding="utf-8"))["fixups"]


def extra_entries():
    if PROFILE is not None:
        return [int(x, 16) for x in PROFILE["extra_entries"]]
    return [int(x, 16) for x in json.loads((CONFIG_DIR / "extra_entries.json").read_text(encoding="utf-8"))["entries"]]


def guest_map():
    """The active profile's guest addresses as {116 address: profile address}; the identity for the baseline."""
    if PROFILE is None:
        return None
    path = GUEST_MAPS_DIR / f"{PROFILE_ID}.json"
    if not path.exists():
        sys.exit(f"{path} is missing: run tools/guest_map.py {PROFILE_ID} --write")
    data = json.loads(path.read_text(encoding="utf-8"))
    return {int(k, 16): int(v["to"], 16) for k, v in data["addresses"].items() if v["class"] in RESOLVED}


RESOLVED = ("identical", "aligned", "data", "decided")


def not_applied(kind):
    """116 addresses of the official-1.1 hooks or overrides this profile's host does not apply (config/patch117_groups.json):
    the build has that code natively or the port has not ported it. Empty for the 1.0.116 lift."""
    if PROFILE is None:
        return set()
    groups = json.loads((CONFIG_DIR / "patch117_groups.json").read_text(encoding="utf-8"))
    return {int(va, 16) for name, g in groups.items() if not PROFILE["patch117"][name] for va in g[kind]}


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def local_config():
    if LOCAL_CONFIG.exists():
        return json.loads(LOCAL_CONFIG.read_text(encoding="utf-8"))
    return {}


def require_target_exe():
    if not GUEST_EXE.exists():
        sys.exit(f"missing {GUEST_EXE}: the original executable is a development input; name it with JPT_PROFILE_EXE")
    actual = sha256_of(GUEST_EXE)
    if actual != TARGET["sha256"]:
        sys.exit(f"{GUEST_EXE} is not the expected build\n  expected {TARGET['sha256']}\n  actual   {actual}")
    return GUEST_EXE


def use_pcrecomp(*subdirs):
    tools = PCRECOMP_DIR / "tools"
    if not tools.is_dir():
        sys.exit(f"missing {PCRECOMP_DIR} -- run tools/fetch_pcrecomp.py first")
    for sub in subdirs:
        sys.path.insert(0, str(tools / sub))
