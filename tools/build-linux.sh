#!/usr/bin/env bash
# Builds the Linux x86_64 executable (and, with --tests, the headless suites). Headless: it compiles and links, it never
# starts the program, a window, a sound or the launcher. One log per run in build/logs/linux_build_NNN.log.
#
#   tools/build-linux.sh [--config RelWithDebInfo|Debug|Release] [--cc gcc|clang] [--no-lifted] [--tests] [--clean]
#                        [--jobs N] [--build-dir DIR] [--sanitize]
# JPT_CMAKE_ARGS adds arguments to the configure step (for example -DFFMPEG_ROOT=<prefix> when FFmpeg is not installed system-wide).
#
# The executable is build/linux-work/bin/trespasser_recomp (or DIR/bin): a work product, never the artifact to run. The artifact is
# build/linux/bin/trespasser_recomp, staged only by tools/test.ps1 (or tools/test-linux.sh --stage) after the gate passed. Needs cmake 3.20+, ninja or make, a C compiler,
# python3, and the pcrecomp checkout in external/pcrecomp (tools/fetch_pcrecomp.py).
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
config=RelWithDebInfo
cc=${CC:-gcc}
lifted=ON
tests=0
clean=0
sanitize=0
jobs=$(nproc 2>/dev/null || echo 4)
build_dir=$repo/build/linux-work

while [ $# -gt 0 ]; do
  case $1 in
    --config) config=$2; shift 2 ;;
    --cc) cc=$2; shift 2 ;;
    --no-lifted) lifted=OFF; shift ;;
    --tests) tests=1; shift ;;
    --clean) clean=1; shift ;;
    --sanitize) sanitize=1; shift ;;
    --jobs) jobs=$2; shift 2 ;;
    --build-dir) mkdir -p "$(dirname "$2")"; build_dir=$(cd "$(dirname "$2")" 2>/dev/null && pwd)/$(basename "$2"); shift 2 ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "build-linux: unknown option $1" >&2; exit 2 ;;
  esac
done

if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
  echo "build-linux: this builds the Linux x86_64 host; this machine is $(uname -s) $(uname -m)" >&2
  exit 2
fi
for tool in cmake python3 "$cc"; do
  command -v "$tool" >/dev/null 2>&1 || { echo "build-linux: $tool is not installed" >&2; exit 2; }
done
generator=Ninja
command -v ninja >/dev/null 2>&1 || generator="Unix Makefiles"
if [ ! -f "$repo/external/pcrecomp/runtime/recomp32/recomp_types.h" ] && [ "$lifted" = ON ]; then
  echo "build-linux: external/pcrecomp is missing: run python3 tools/fetch_pcrecomp.py (or pass --no-lifted)" >&2
  exit 2
fi

mkdir -p "$repo/build/logs"
n=1
while [ -e "$(printf '%s/build/logs/linux_build_%03d.log' "$repo" "$n")" ]; do n=$((n + 1)); done
log=$(printf '%s/build/logs/linux_build_%03d.log' "$repo" "$n")
{
  echo "linux build: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "host: $(uname -srm), glibc $(ldd --version 2>&1 | head -1 | awk '{print $NF}')"
  echo "compiler: $("$cc" --version | head -1)"
  echo "cmake: $(cmake --version | head -1), generator $generator"
  echo "config $config, lifted $lifted, build dir $build_dir"
} | tee "$log"

python3 -I "$repo/tools/fetch_launcher_deps.py" >>"$log" 2>&1 || {
  echo "build-linux: the launcher and SDL sources (config/launcher_deps.txt) are missing and could not be fetched: run python3 tools/fetch_launcher_deps.py" >&2
  tail -5 "$log" >&2
  exit 2
}
[ "$clean" = 1 ] && rm -rf "$build_dir"
export CC=$cc
case "$cc" in
  clang*) export CXX=${cc/clang/clang++} ;;
  gcc*) export CXX=${cc/gcc/g++} ;;
  *) export CXX=c++ ;;
esac
sanitize_args=()
[ "$sanitize" = 1 ] && sanitize_args=(-DJPT_LINUX_SANITIZE=ON)
targets=(trespasser_recomp)
[ "$lifted" = ON ] && targets+=(jpt_lifted_linux)
[ "$tests" = 1 ] && targets+=(jpt_linux_tests)

{
  # shellcheck disable=SC2086
  cmake -S "$repo" -B "$build_dir" -G "$generator" -DCMAKE_BUILD_TYPE="$config" -DJPT_LINUX_LIFTED="$lifted" "${sanitize_args[@]}" ${JPT_CMAKE_ARGS:-}
  cmake --build "$build_dir" --parallel "$jobs" --target "${targets[@]}"
} >>"$log" 2>&1 || {
  echo "build-linux: FAILED, last lines of $log:" >&2
  tail -40 "$log" >&2
  exit 1
}
warnings=$(grep -c "warning:" "$log" || true)
echo "build-linux: OK, $warnings warnings, log $log"
echo "executable: $build_dir/bin/trespasser_recomp"
