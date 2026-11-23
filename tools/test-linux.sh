#!/usr/bin/env bash
# The Linux gate: builds the headless suites and the executable incrementally, then runs every portable and Linux-specific suite, the
# Python suites and tools, and the static checks of the executable (tools/linux_gate.py). Headless: SDL's dummy audio and video
# drivers, no window, no sound, no launcher. The executable is inspected and asked only for --build-info and --self-check (headless). tools/test.ps1 runs this through WSL;
# running it by hand gives the same result.
#
#   tools/test-linux.sh [--build-dir DIR | --native-build] [--no-build] [--config CONFIG] [--cc COMPILER] [--elf-baseline 2.31]
#                       [--machine] [--filter TEXT] [--list] [--sanitize] [--stage]
#
# --native-build builds under ~/.cache/jpt-recomp, on the Linux file system, which is much faster than a Windows drive in WSL.
# --stage copies the validated executable to build/linux/bin (with its build manifest) when every row passed.
# --sanitize builds a separate AddressSanitizer + UndefinedBehaviorSanitizer tree and runs only the Linux-specific suites in it.
# Exit code 0 when every row passes; a suite that needs data that is absent says DATA UNAVAILABLE and is never counted as a pass.
set -uo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=$repo/build/linux-work
build=1
config=RelWithDebInfo
baseline=2.31
cc=${CC:-}
machine=()
filter=()
list=0
native=0
sanitize=0
stage=0
while [ $# -gt 0 ]; do
  case $1 in
    --build-dir) build_dir=$2; shift 2 ;;
    --native-build) native=1; shift ;;
    --no-build) build=0; shift ;;
    --config) config=$2; shift 2 ;;
    --cc) cc=$2; shift 2 ;;
    --elf-baseline) baseline=$2; shift 2 ;;
    --machine) machine=(--machine); shift ;;
    --filter) filter=(--filter "$2"); shift 2 ;;
    --list) list=1; shift ;;
    --sanitize) sanitize=1; shift ;;
    --stage) stage=1; shift ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "test-linux: unknown option $1" >&2; exit 2 ;;
  esac
done

emit() { [ ${#machine[@]} -gt 0 ] && echo "@@$*" || echo "  $*"; }

if [ -z "$cc" ]; then
  for candidate in gcc-13 gcc-12 gcc-11 gcc clang; do
    command -v "$candidate" >/dev/null 2>&1 && { cc=$candidate; break; }
  done
fi
[ -n "$cc" ] || { emit "BUILD|FAIL|no C compiler (gcc, gcc-13, gcc-12, gcc-11 or clang) is installed"; exit 2; }

if [ "$native" = 1 ]; then
  id=$(. /etc/os-release 2>/dev/null; echo "${ID:-linux}-${VERSION_ID:-0}")
  build_dir=${XDG_CACHE_HOME:-$HOME/.cache}/jpt-recomp/$id-$cc
  [ "$sanitize" = 1 ] && build_dir=$build_dir-san
fi
mkdir -p "$build_dir"

emit "INFO|compiler|$("$cc" --version | head -1)"
emit "INFO|cmake|$(cmake --version 2>/dev/null | head -1)"
emit "INFO|build_dir|$build_dir"

[ "$list" = 1 ] && exec python3 -I "$repo/tools/linux_gate.py" --build-dir "$build_dir" --list "${filter[@]}"

# Smacker video needs FFmpeg's libavformat, libavcodec and libavutil. When the machine has none, a pinned LGPL-only build is made once
# into the cache (tools/ffmpeg-bootstrap.sh: no sudo, nothing installed, the commit is verified); without network the video rows say BLOCKED.
# JPT_FFMPEG_PINNED=1 (the release packaging) always uses that build and fails when it cannot be made.
if [ "${JPT_FFMPEG_PINNED:-0}" = 1 ] ||
   { [ "${JPT_FFMPEG_BOOTSTRAP:-1}" = 1 ] && [ -z "${JPT_CMAKE_ARGS:-}" ] && ! pkg-config --exists libavformat libavcodec libavutil 2>/dev/null; }; then
  started=$(date +%s)
  if ffmpeg_prefix=$(bash "$repo/tools/ffmpeg-bootstrap.sh" --cc "$cc" 2>/tmp/jpt-ffmpeg-bootstrap.$$); then
    emit "INFO|ffmpeg|$(basename "$ffmpeg_prefix") ($(( $(date +%s) - started )) s)"
    export JPT_CMAKE_ARGS="-DFFMPEG_ROOT=$ffmpeg_prefix"
  else
    code=$?
    emit "NOTE|ffmpeg bootstrap did not complete (exit $code): $(tail -1 /tmp/jpt-ffmpeg-bootstrap.$$ | cut -c1-200); the Smacker video rows are BLOCKED"
    if [ "${JPT_FFMPEG_PINNED:-0}" = 1 ]; then
      rm -f /tmp/jpt-ffmpeg-bootstrap.$$
      emit "FAIL|ffmpeg|the release needs the pinned LGPL-only FFmpeg build"
      exit 2
    fi
  fi
  rm -f /tmp/jpt-ffmpeg-bootstrap.$$
fi

# A release must ship an LGPL-only FFmpeg, whether it came from the bootstrap or from the machine.
if [ "${JPT_FFMPEG_REQUIRE_LGPL:-0}" = 1 ]; then
  export JPT_CMAKE_ARGS="${JPT_CMAKE_ARGS:-} -DJPT_FFMPEG_REQUIRE_LGPL=ON"
fi

if [ "$build" = 1 ]; then
  started=$(date +%s)
  extra=()
  [ "$sanitize" = 1 ] && extra=(--sanitize)
  if output=$(bash "$repo/tools/build-linux.sh" --tests --config "$config" --cc "$cc" --build-dir "$build_dir" "${extra[@]}" 2>&1); then
    emit "BUILD|PASS|$(( $(date +%s) - started ))|$(echo "$output" | grep -E '^build-linux: OK' | head -1)"
  else
    emit "BUILD|FAIL|$(( $(date +%s) - started ))|build failed"
    echo "$output" | tail -40 | sed 's/^/@@OUT|build|/'
    exit 1
  fi
fi

if [ "$sanitize" = 1 ]; then
  export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
fi
gate_args=()
[ "$sanitize" = 1 ] && gate_args=(--sanitize)
python3 -I "$repo/tools/linux_gate.py" --build-dir "$build_dir" --elf-baseline "$baseline" "${gate_args[@]}" "${machine[@]}" "${filter[@]}"
code=$?
if [ "$stage" = 1 ] && [ $code -eq 0 ] && [ "$sanitize" = 0 ] && [ ${#filter[@]} -eq 0 ]; then
  python3 -I "$repo/tools/linux_artifact.py" stage --elf "$build_dir/bin/trespasser_recomp" --dest-dir "$repo/build/linux/bin" || code=$?
fi
exit $code
