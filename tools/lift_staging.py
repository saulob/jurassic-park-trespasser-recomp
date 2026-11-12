"""Staging, publishing and cleanup of the lifted C (tools/run_lift.py).

The lift is written into a staging folder under build\\lift-staging, validated there, and only then published into
src\\recomp\\gen file by file (changed files only, each through a temporary file and os.replace). The stats file is
written last and marks the lift complete. Cleaning the staging folder afterwards is best effort: a scanner or an
indexer that still holds a file of it never turns a finished lift into a failure.
"""
import errno
import json
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

from common import ROOT

STAGING_ROOT = ROOT / "build" / "lift-staging"
STAGING_PREFIX = "lift-"
LEGACY_PREFIX = ".lift-"
COMPLETE_SUFFIX = ".complete"
RETRY_DELAYS = (0.05, 0.1, 0.2, 0.4, 0.8)
ABANDONED_AFTER_SECONDS = 3600
WIN_ACCESS_DENIED, WIN_SHARING_VIOLATION = 5, 32


class LiftPublishError(Exception):
    pass


def is_busy(error):
    """A file another process holds for a moment: access denied or a sharing violation."""
    if getattr(error, "winerror", None) in (WIN_ACCESS_DENIED, WIN_SHARING_VIOLATION):
        return True
    return os.name != "nt" and isinstance(error, PermissionError) and error.errno == errno.EACCES


def retrying(action, delays=RETRY_DELAYS, sleep=time.sleep):
    """Runs action(); a busy file is retried after each delay, any other error is raised at once."""
    for delay in delays:
        try:
            return action()
        except OSError as error:
            if not is_busy(error):
                raise
        sleep(delay)
    return action()


def create_staging(root=None):
    root = Path(root or STAGING_ROOT)
    root.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix=STAGING_PREFIX, dir=root))


def _remove_tree(path):
    try:
        shutil.rmtree(path)
    except FileNotFoundError:
        pass


def remove_tree(path, delays=RETRY_DELAYS, sleep=time.sleep):
    """True when path is gone. A busy file is retried with a short backoff; the last error is returned otherwise."""
    try:
        retrying(lambda: _remove_tree(path), delays, sleep)
    except OSError as error:
        return error
    return True


def marker_for(path):
    """Beside the folder, not in it: a failed removal has already deleted whatever it could."""
    path = Path(path)
    return path.with_name(path.name + COMPLETE_SUFFIX)


def finish_staging(path, delays=RETRY_DELAYS, sleep=time.sleep, out=sys.stdout):
    """Best-effort cleanup after the outputs are final; True when the folder is gone."""
    path = Path(path)
    try:
        marker_for(path).write_text("", encoding="ascii")
    except OSError:
        pass
    result = remove_tree(path, delays, sleep)
    if result is True:
        try:
            marker_for(path).unlink(missing_ok=True)
        except OSError:
            pass
        return True
    print(f"[!] warning: the staging folder {path} could not be removed ({result}); a later lift removes it", file=out)
    return False


def sweep_staging(root=None, keep=None, legacy_dir=None, now=None, delays=(), out=sys.stdout):
    """Removes the folders earlier lifts left: finished ones at once, others once abandoned, and the old in-tree ones.
    Only direct children of the two folders with the lift prefix are considered, never the generated sources."""
    root = Path(root or STAGING_ROOT)
    now = time.time() if now is None else now
    removed = 0
    candidates = []
    if root.is_dir():
        for child in root.iterdir():
            if child.is_dir() and child.name.startswith(STAGING_PREFIX) and child != keep:
                finished = marker_for(child).exists()
                candidates.append((child, finished or now - child.stat().st_mtime > ABANDONED_AFTER_SECONDS))
    if legacy_dir and Path(legacy_dir).is_dir():
        candidates += [(child, True) for child in Path(legacy_dir).iterdir()
                       if child.is_dir() and child.name.startswith(LEGACY_PREFIX)]
    for child, stale in candidates:
        if stale and remove_tree(child, delays) is True:
            marker_for(child).unlink(missing_ok=True)
            removed += 1
    if root.is_dir():
        for marker in root.glob(STAGING_PREFIX + "*" + COMPLETE_SUFFIX):
            if not marker.with_name(marker.name[:-len(COMPLETE_SUFFIX)]).exists():
                marker.unlink(missing_ok=True)
    if removed:
        print(f"[*] removed {removed} staging folder(s) left by earlier lifts", file=out)
    return removed


def _same_content(a, b):
    try:
        return a.stat().st_size == b.stat().st_size and a.read_bytes() == b.read_bytes()
    except FileNotFoundError:
        return False


def _publish_file(src, dst, delays, sleep):
    temporary = dst.with_name(dst.name + ".new")
    try:
        shutil.copyfile(src, temporary)
        retrying(lambda: os.replace(temporary, dst), delays, sleep)
    except OSError as error:
        raise LiftPublishError(f"cannot replace {dst}: {error}") from error
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        except OSError:
            pass


def publish(staged, out, stats_path, stats_text, pattern="recomp_*", delays=RETRY_DELAYS, sleep=time.sleep):
    """Makes the validated staged files the contents of out and writes the stats last. Returns the number of files
    written; unchanged files are left alone, files of the pattern that were not produced are removed. Any failure
    raises LiftPublishError and leaves no stats file, so the lift counts as incomplete."""
    staged, out, stats_path = Path(staged), Path(out), Path(stats_path)
    out.mkdir(parents=True, exist_ok=True)
    produced = sorted(p for p in staged.glob(pattern) if p.is_file())
    if not produced:
        raise LiftPublishError(f"nothing was produced in {staged}")
    try:
        retrying(lambda: stats_path.unlink(missing_ok=True), delays, sleep)
    except OSError as error:
        raise LiftPublishError(f"cannot remove the previous {stats_path}: {error}") from error
    written = 0
    for src in produced:
        dst = out / src.name
        if _same_content(src, dst):
            continue
        _publish_file(src, dst, delays, sleep)
        written += 1
    names = {p.name for p in produced}
    for stale in sorted(out.glob(pattern)):
        if stale.name not in names:
            try:
                retrying(stale.unlink, delays, sleep)
            except OSError as error:
                raise LiftPublishError(f"cannot remove {stale}: {error}") from error
    try:
        if not isinstance(json.loads(stats_text), dict):
            raise ValueError("not an object")
    except ValueError as error:
        raise LiftPublishError(f"the lift stats are not valid: {error}") from error
    stats_path.parent.mkdir(parents=True, exist_ok=True)
    pending = stats_path.with_name(stats_path.name + ".new")
    try:
        pending.write_text(stats_text, encoding="utf-8")
        retrying(lambda: os.replace(pending, stats_path), delays, sleep)
    except OSError as error:
        raise LiftPublishError(f"cannot write {stats_path}: {error}") from error
    finally:
        try:
            pending.unlink()
        except OSError:
            pass
    return written
