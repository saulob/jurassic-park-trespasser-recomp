#!/usr/bin/env bash
# Starts the AppImage three ways with --build-info, which prints the build's JSON and opens no window, no audio device and
# no log: the extracted AppRun, --appimage-extract-and-run, and the file itself (a FUSE mount where FUSE works, otherwise
# the patched runtime's extract-and-run fallback). A missing glibc version or library stops a start; a start must also
# leave nothing inside the extracted contents and no extraction folder behind. The game and the launcher are never started.
#
#   packaging/linux/smoke-test.sh path/to/JurassicParkTrespasserRecompiled-*.AppImage
set -euo pipefail
appimage=$(readlink -f -- "${1:?usage: packaging/linux/smoke-test.sh FILE.AppImage}")
[[ -f $appimage ]] || { echo "FAIL: missing $appimage" >&2; exit 1; }
version=$(basename -- "$appimage" | sed -nE 's/.*-v([0-9]+\.[0-9]+\.[0-9]+)-x86_64\.AppImage$/\1/p')
[[ -n $version ]] || { echo "FAIL: $appimage is not named <asset>-v<version>-x86_64.AppImage" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/home" "$work/beside" "$work/tmp"
cp -- "$appimage" "$work/beside/game.AppImage"
chmod 755 "$work/beside/game.AppImage"
cd "$work"
export HOME=$work/home TMPDIR=$work/tmp SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
unset DISPLAY WAYLAND_DISPLAY APPIMAGE_EXTRACT_AND_RUN

"$work/beside/game.AppImage" --appimage-extract >/dev/null
[[ -x squashfs-root/AppRun && -x squashfs-root/usr/bin/trespasser_recomp ]] || { echo 'FAIL: --appimage-extract' >&2; exit 1; }
find squashfs-root | sort > before.txt

check_start() { # label command...
    local label=$1 status=0
    shift
    "$@" > start.log 2>&1 || status=$?
    if ((status != 0)) || grep -qE 'GLIBC_|GLIBCXX|CXXABI|error while loading|cannot open shared object' start.log; then
        cat start.log >&2
        echo "FAIL ($label): exit $status" >&2
        exit 1
    fi
    grep -Eq "\"version\":[[:space:]]*\"$version\"" start.log || { cat start.log >&2; echo "FAIL ($label): --build-info does not report version $version" >&2; exit 1; }
    echo "OK ($label)"
}

check_start 'extracted AppRun' env APPIMAGE="$work/beside/game.AppImage" squashfs-root/AppRun --build-info
diff <(find squashfs-root | sort) before.txt >/dev/null || { echo 'FAIL: data was written inside the AppImage contents' >&2; exit 1; }
check_start '--appimage-extract-and-run' "$work/beside/game.AppImage" --appimage-extract-and-run --build-info
if [[ -e /dev/fuse ]] && { command -v fusermount || command -v fusermount3; } >/dev/null; then
    mode='FUSE mount'
else
    mode='no FUSE: the runtime falls back to extract-and-run'
fi
check_start "the AppImage file ($mode)" "$work/beside/game.AppImage" --build-info

if [[ -n $(find "$work/tmp" -mindepth 1 -maxdepth 1 -name 'appimage_extracted_*' 2>/dev/null) ]]; then
    echo 'FAIL: the runtime left its extraction folder behind' >&2
    exit 1
fi
[[ -z $(find "$work/beside" -mindepth 1 -maxdepth 1 ! -name game.AppImage ! -name squashfs-root 2>/dev/null) ]] ||
    { echo 'FAIL: a start wrote files beside the AppImage' >&2; exit 1; }
echo 'OK: every start mode ran headless and left nothing behind'
