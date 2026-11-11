"""Paths and fingerprints shared by the project tools."""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG_DIR = ROOT / "config"
LOCAL_CONFIG = CONFIG_DIR / "local.json"
PCRECOMP_DIR = ROOT / "external" / "pcrecomp"
GAME_DIR = ROOT / "game"
ANALYSIS_DIR = ROOT / "analysis"
GEN_DIR = ROOT / "src" / "recomp" / "gen"

TARGET = json.loads((CONFIG_DIR / "target.json").read_text(encoding="utf-8"))
GUEST_EXE = GAME_DIR / TARGET["installed_name"]


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
        sys.exit(f"missing {GUEST_EXE} -- run tools/extract_game.py first")
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
