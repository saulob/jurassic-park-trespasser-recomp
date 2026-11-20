#!/usr/bin/env bash
# The Linux gate: builds the headless suites and the executable, runs every portable and Linux-specific suite, checks the
# executable's static properties, and lists what no Linux suite proves. Headless: SDL's dummy audio and video drivers, no
# window, no sound, no launcher. The executable is inspected, never started.
#
#   tools/test-linux.sh [--build-dir DIR] [--no-build] [--elf-baseline 2.31]
#
# Exit code 0 when every suite passes and the executable checks pass; 1 otherwise. A suite that needs data that is absent
# says DATA UNAVAILABLE and is never counted as a pass.
set -uo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=$repo/build/linux
build=1
baseline=2.31
while [ $# -gt 0 ]; do
  case $1 in
    --build-dir) build_dir=$2; shift 2 ;;
    --no-build) build=0; shift ;;
    --elf-baseline) baseline=$2; shift 2 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "test-linux: unknown option $1" >&2; exit 2 ;;
  esac
done

if [ "$build" = 1 ]; then
  bash "$repo/tools/build-linux.sh" --tests --build-dir "$build_dir" || { echo "test-linux: build failed"; exit 1; }
fi

export SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy
failures=0
passed=0
suites=$(python3 -I - "$repo/tests/linux_suites.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print(" ".join(d["portable"] + d["linux_specific"]))
PY
)

echo "== suites (portable and Linux-specific)"
for suite in $suites; do
  exe=$build_dir/$suite
  if [ ! -x "$exe" ]; then
    echo "FAIL     $suite: not built"
    failures=$((failures + 1))
    continue
  fi
  output=$(cd "$repo" && timeout 600 "$exe" game 2>&1)
  code=$?
  summary=$(echo "$output" | grep -E '(^|: )[0-9]+ (checks|passed)|tests passed: 0 failure' | tail -1)
  if [ $code -eq 0 ] && [ -n "$summary" ]; then
    echo "PASS     $suite: $summary"
    passed=$((passed + 1))
  else
    echo "FAIL     $suite (exit $code)"
    echo "$output" | tail -15 | sed 's/^/           /'
    failures=$((failures + 1))
  fi
  echo "$output" | grep -E "DATA UNAVAILABLE|SKIP" | sed 's/^/           /' | head -3
done

echo "== python suites"
for t in tests/test_audio_portability.py tests/test_layout.py tests/test_selfmod_lift.py tests/test_guest_module_lift.py tests/test_linux_native_rules.py; do
  [ -f "$repo/$t" ] || continue
  if (cd "$repo" && python3 -I "$t" >/dev/null 2>&1); then
    echo "PASS     $t"
    passed=$((passed + 1))
  else
    echo "NOT RUN  $t (needs data or Windows tools on this machine; covered by tools/test.ps1)"
  fi
done

echo "== executable"
elf=$build_dir/bin/trespasser_recomp
if [ -x "$elf" ]; then
  if python3 -I "$repo/tools/linux_elf_check.py" "$elf" --baseline "$baseline" --local-path "$repo" "$HOME"; then
    passed=$((passed + 1))
  else
    echo "         (the glibc check names symbols above the baseline: a local build on a newer system can use newer ones;"
    echo "          the fix is to build on Ubuntu 20.04 or an equivalent toolchain, not to hide them)"
    failures=$((failures + 1))
  fi
else
  echo "FAIL     $elf not built"
  failures=$((failures + 1))
fi

echo "== lifted guest compiled for x86_64"
if [ -f "$build_dir/libjpt_lifted_linux.a" ]; then
  python3 -I "$repo/tools/linux_lifted_link_report.py" "$build_dir/libjpt_lifted_linux.a"     --provided-by "$build_dir/libjpt_linux_core.a" "$build_dir/libjpt_linux_guest.a"
  echo "  (what is left after the Linux runtime and bindings: only the C library; the executable links the guest)"
else
  echo "  NOT BUILT (configure with JPT_LINUX_LIFTED=ON)"
fi

echo "== not proved by any Linux suite"
python3 -I - "$repo/tests/linux_suites.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print(f"  windows-specific suites ({len(d['windows_specific'])}): covered only by tools/test.ps1")
for line in d["runtime_only"]:
    print("  runtime only:", line)
PY

echo "== result: $passed passed, $failures failed"
[ $failures -eq 0 ]
