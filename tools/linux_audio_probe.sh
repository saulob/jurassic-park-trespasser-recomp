#!/usr/bin/env bash
# Builds and runs the Linux audio probe (tests/linux) with GCC and, when present, Clang. Headless: SDL's dummy audio
# driver, no window, no sound. Builds outside the repository (JPT_PROBE_BUILD, default ~/.cache/jpt_audio_probe_<cc>).
# FFmpeg for the Smacker boundary test: pkg-config, or JPT_FFMPEG_ROOT (include/ and lib/); otherwise that suite says
# DATA UNAVAILABLE.
#   usage: tools/linux_audio_probe.sh <repository root>
set -u
repo=${1:?usage: linux_audio_probe.sh <repository root>}
export SDL_AUDIODRIVER=dummy
total=0
failures=0
ran=0
tests="audio_portable_tests audio_backend_tests dsound_compat_tests hammond_voiceover_tests smacker_tests smacker_ffmpeg_tests"

for cc in gcc clang; do
  if ! command -v "$cc" >/dev/null 2>&1; then
    echo "linux audio probe: $cc: skipped - DATA UNAVAILABLE ($cc is not installed)"
    continue
  fi
  build=${JPT_PROBE_BUILD:-$HOME/.cache/jpt_audio_probe}_$cc
  mkdir -p "$(dirname "$build")"
  extra=()
  [ -n "${JPT_FFMPEG_ROOT:-}" ] && extra+=("-DFFMPEG_ROOT=$JPT_FFMPEG_ROOT")
  if ! cmake -S "$repo/tests/linux" -B "$build" -G Ninja -DCMAKE_C_COMPILER="$cc" "${extra[@]}" >"$build.configure.log" 2>&1; then
    echo "FAIL linux audio probe: $cc: configure failed"
    tail -20 "$build.configure.log"
    failures=$((failures + 1))
    continue
  fi
  if ! ninja -C "$build" $tests jpt_audio_portable >"$build.build.log" 2>&1; then
    echo "FAIL linux audio probe: $cc: build failed"
    tail -40 "$build.build.log"
    failures=$((failures + 1))
    continue
  fi
  warnings=$(grep -c "warning:" "$build.build.log")
  echo "linux audio probe: $cc $("$cc" -dumpfullversion 2>/dev/null || "$cc" -dumpversion), $warnings warnings (built with -Werror)"
  for t in $tests; do
    output=$("$build/$t" 2>&1)
    code=$?
    echo "$output" | sed "s/^/  [$cc] /"
    line=$(echo "$output" | grep -E '^[a-z_]+: [0-9]+ checks, [0-9]+ failures' | tail -1)
    if [ $code -ne 0 ] || [ -z "$line" ]; then
      echo "FAIL linux audio probe: $cc: $t exited with $code"
      failures=$((failures + 1))
      continue
    fi
    checks=$(echo "$line" | sed -E 's/^[a-z_]+: ([0-9]+) checks.*/\1/')
    total=$((total + checks))
    ran=$((ran + 1))
  done
done

if [ $ran -eq 0 ]; then
  echo "linux audio probe: skipped - DATA UNAVAILABLE (no Linux compiler could build it)"
  exit $failures
fi
echo "linux audio probe: $ran test programs, $total checks, $failures failures"
exit $failures
