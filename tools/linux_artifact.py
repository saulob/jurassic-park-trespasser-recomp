#!/usr/bin/env python3
"""The one Linux executable of this port: how the gate proves it, and how it becomes the artifact everything else uses.

    linux_artifact.py check  --elf ELF [--root DIR]
    linux_artifact.py stage  --elf ELF --dest-dir DIR [--result-json FILE] [--root DIR]
    linux_artifact.py verify --elf ELF [--manifest FILE] [--root DIR]

check   runs the binary's own --build-info and --self-check (headless: no window, no audio, no guest code) and compares them with the
        sources (source fingerprint, feature contract hash, override and hook lists) and with tests/feature_contract.json (every
        Linux PASS feature that names runtime capabilities needs them in the binary).
stage   check, then copies the binary next to a build manifest into DIR atomically (trespasser_recomp.new, then rename). A binary that
        fails the check never replaces the last validated one.
verify  the staged binary still is what its manifest says (SHA-256, current sources, self-check). Used before packaging.

The canonical artifact is build/linux/bin/trespasser_recomp with trespasser_recomp.build.json beside it; build/ is the gate's and the
packager's, not a CMake tree (tools/build-linux.sh builds under build/linux-work by default)."""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import source_fingerprint  # noqa: E402

ROOT = HERE.parent
ARTIFACT_NAME = "trespasser_recomp"
MANIFEST_NAME = "trespasser_recomp.build.json"
HEADLESS = {"SDL_AUDIODRIVER": "dummy", "SDL_VIDEODRIVER": "dummy", "JPT_AUDIO_HEADLESS": "1"}


class Result:
    def __init__(self, name, ok, summary, lines=None):
        self.name, self.ok, self.summary, self.lines = name, ok, summary, lines or []


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def contract_sha256(root):
    return hashlib.sha256((root / "tests" / "feature_contract.json").read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def run_headless(elf, argument):
    """The binary's JSON for argument, the exit code, and the files it left in an empty working folder."""
    env = {key: value for key, value in os.environ.items() if not key.startswith(("JPT_", "TRESPASSER_"))}
    env.update(HEADLESS)
    with tempfile.TemporaryDirectory(prefix="jpt-selfcheck-") as folder:
        done = subprocess.run([str(elf), argument], cwd=folder, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180)
        left = sorted(os.listdir(folder))
    try:
        data = json.loads(done.stdout.decode("utf-8"))
    except ValueError:
        data = None
    return data, done.returncode, left, done.stderr.decode("utf-8", "replace")


def list_count(path, pattern):
    return len(re.findall(pattern, path.read_text(encoding="utf-8"), re.M))


def glibc_max(elf):
    try:
        text = subprocess.run(["readelf", "--version-info", "-W", str(elf)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout.decode("utf-8", "replace")
    except OSError:
        return None
    versions = re.findall(r"Name: GLIBC_(\d+(?:\.\d+)*)", text)
    return max(versions, key=lambda v: [int(p) for p in v.split(".")]) if versions else None


def check_elf(elf, root=ROOT):
    """Rows of the checks of a binary: self-check, provenance against the sources, the feature contract."""
    results = []
    info, code, left, error = run_headless(elf, "--build-info")
    if info is None or code != 0:
        return [Result("elf self-check", False, "--build-info gave no JSON (exit %d)" % code, error.splitlines()[-10:])]
    selfcheck, code, left2, error = run_headless(elf, "--self-check")
    if selfcheck is None:
        return [Result("elf self-check", False, "--self-check gave no JSON (exit %d)" % code, error.splitlines()[-10:])]
    caps = selfcheck.get("capabilities", {})
    failed = [name for name, entry in caps.items() if not entry.get("ok")]
    problems = []
    if code != 0 or not selfcheck.get("self_check"):
        problems.append("exit %d, self_check %s" % (code, selfcheck.get("self_check")))
    problems += ["capability %s: %s" % (name, caps[name].get("detail", "")) for name in failed]
    if left or left2:
        problems.append("the self-check wrote files into its working folder: %s" % ", ".join(left + left2))
    if selfcheck.get("build") != info.get("build"):
        problems.append("--build-info and --self-check disagree about the build")
    reg = selfcheck.get("registry", {})
    results.append(Result("elf self-check", not problems, "%d capabilities, overrides %s of %s applicable, %s/%s lift hooks, no window, audio or file"
                          % (selfcheck.get("capability_count", 0), reg.get("overrides_linux_applicable"), reg.get("override_registry"), reg.get("lift_hooks_active"), reg.get("lift_hooks")), problems))

    build = selfcheck.get("build", {})
    problems = []
    current = source_fingerprint.fingerprint(root)
    if build.get("source_fingerprint") != current:
        problems.append("the binary was built from other sources: its fingerprint %s, the tree's %s" % (build.get("source_fingerprint"), current))
    if build.get("contract_sha256") != contract_sha256(root):
        problems.append("the binary carries another feature contract than tests/feature_contract.json")
    listed = list_count(root / "src" / "host" / "guest_override_list.h", r"^GUEST_OVERRIDE\(")
    hooks = list_count(root / "src" / "host" / "lift_hook_list.h", r"^GUEST_HOOK\(|^LIFT_HOOK\(")
    if reg.get("override_registry") != listed:
        problems.append("the binary lists %s overrides, guest_override_list.h %d" % (reg.get("override_registry"), listed))
    if hooks and reg.get("lift_hooks") != hooks:
        problems.append("the binary has %s lift hooks, lift_hook_list.h %d" % (reg.get("lift_hooks"), hooks))
    if build.get("config") == "Release" and build.get("bug_report_transport") != "openssl":
        problems.append("a Release build without the Bug Report HTTPS transport (OpenSSL): the Bug Report page could not send")
    max_glibc = glibc_max(elf)
    results.append(Result("elf build provenance", not problems, "fingerprint %s, %s, glibc %s, FFmpeg %s" % (
        build.get("source_fingerprint", "?")[:12], build.get("compiler"), max_glibc, "LGPL" if build.get("ffmpeg", {}).get("lgpl_only") else "not LGPL-only"), problems))

    contract = json.loads((root / "tests" / "feature_contract.json").read_text(encoding="utf-8"))
    problems, required, features = [], set(), 0
    for feature in contract["features"]:
        section = feature.get("linux", {})
        if section.get("status") != "PASS" or not section.get("runtime"):
            continue
        features += 1
        for name in section["runtime"]:
            required.add(name)
            if not caps.get(name, {}).get("ok"):
                problems.append("feature %s needs capability %s, which the binary does not hold" % (feature["id"], name))
    results.append(Result("elf feature contract", not problems, "%d Linux PASS features name %d runtime capabilities, all present" % (features, len(required)) if not problems else "%d missing" % len(problems), problems))
    return results


def print_results(results):
    for r in results:
        print("%-5s %-24s %s" % ("PASS" if r.ok else "FAIL", r.name, r.summary))
        for line in r.lines:
            print("        " + line)


def write_atomic(path, data_writer, mode):
    temp = path.with_name(path.name + ".new")
    if temp.exists():
        temp.unlink()
    data_writer(temp)
    os.chmod(temp, mode)
    with open(temp, "r+b") as handle:
        os.fsync(handle.fileno())
    os.replace(temp, path)


def stage(elf, dest, result_json, root):
    results = check_elf(elf, root)
    print_results(results)
    if not all(r.ok for r in results):
        print("stage: the binary is not validated; %s keeps its last validated binary" % dest)
        return 1
    selfcheck, _, _, _ = run_headless(elf, "--self-check")
    sha = sha256_file(elf)
    gate = {}
    if result_json and Path(result_json).is_file():
        gate = json.loads(Path(result_json).read_text(encoding="utf-8-sig"))
    manifest = {
        "schema": 1,
        "elf_sha256": sha,
        "elf_size": elf.stat().st_size,
        "source_fingerprint": selfcheck["build"]["source_fingerprint"],
        "feature_contract_sha256": selfcheck["build"]["contract_sha256"],
        "build": {key: selfcheck["build"][key] for key in ("version", "git_commit", "git_dirty", "compiler", "target", "config", "glibc_baseline")},
        "glibc_max": glibc_max(elf),
        "ffmpeg": selfcheck["build"]["ffmpeg"],
        "self_check": {"capabilities": selfcheck["capability_count"], "registry": selfcheck["registry"], "audio": selfcheck["audio"]},
        "gate": gate,
        "test_result": "PASS",
        "gate_timestamp_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    dest.mkdir(parents=True, exist_ok=True)
    write_atomic(dest / MANIFEST_NAME, lambda p: p.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8"), 0o644)
    write_atomic(dest / ARTIFACT_NAME, lambda p: shutil.copyfile(elf, p), 0o755)
    print("stage: %s  sha256 %s" % (dest / ARTIFACT_NAME, sha))
    return 0


def verify(elf, manifest_path, root):
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    results = []
    sha = sha256_file(elf)
    problems = []
    if sha != manifest.get("elf_sha256"):
        problems.append("the binary is %s, its manifest says %s" % (sha, manifest.get("elf_sha256")))
    if manifest.get("test_result") != "PASS":
        problems.append("the manifest does not record a passing gate")
    if manifest.get("source_fingerprint") != source_fingerprint.fingerprint(root):
        problems.append("the sources changed since the binary was validated")
    results.append(Result("artifact manifest", not problems, "sha256 %s" % sha[:16], problems))
    results += check_elf(elf, root)
    print_results(results)
    return 0 if all(r.ok for r in results) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("check", "stage", "verify"))
    parser.add_argument("--elf", required=True)
    parser.add_argument("--dest-dir")
    parser.add_argument("--manifest")
    parser.add_argument("--result-json")
    parser.add_argument("--root", default=str(ROOT))
    args = parser.parse_args()
    elf, root = Path(args.elf), Path(args.root)
    if not elf.is_file():
        print("no binary at %s" % elf)
        return 1
    if args.command == "check":
        results = check_elf(elf, root)
        print_results(results)
        return 0 if all(r.ok for r in results) else 1
    if args.command == "stage":
        if not args.dest_dir:
            parser.error("stage needs --dest-dir")
        return stage(elf, Path(args.dest_dir), args.result_json, root)
    return verify(elf, Path(args.manifest) if args.manifest else elf.with_name(MANIFEST_NAME), root)


if __name__ == "__main__":
    sys.exit(main())
