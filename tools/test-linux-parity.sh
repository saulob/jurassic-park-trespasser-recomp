#!/usr/bin/env bash
# The Linux feature parity gate: the static contracts first (application ID, portable folders, AppImage contract, launcher and setting
# options, the written inventory), then the whole headless Linux gate, whose rows include the parity suites (port_tests, app_identity_tests,
# linux_paths_tests, linux_launcher_tests, linux_present_tests, linux_sdl_window_tests). Headless: no window, no game, no launcher, no sound.
#
#   tools/test-linux-parity.sh [--static-only] [options of tools/test-linux.sh]
#
# --static-only runs only the Python contracts: no compiler, no build, a few seconds. tools\test.ps1 runs the full gate through WSL.
set -uo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
static_only=0
args=()
for arg in "$@"; do
  case $arg in
    --static-only) static_only=1 ;;
    -h|--help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) args+=("$arg") ;;
  esac
done

python3 -I "$repo/tools/linux_parity_report.py" --check || exit 1
(cd "$repo" && python3 -I -B -m unittest discover -s tests -p "test_linux_parity.py") || exit 1
echo "linux parity: static contracts passed"
[ "$static_only" = 1 ] && exit 0
exec "$repo/tools/test-linux.sh" "${args[@]}"
