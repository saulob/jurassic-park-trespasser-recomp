#!/usr/bin/env bash
# Takes the Linux executable the gate validated and prepares it for packaging; it never builds. Packaging (AppImage, Steam Deck) later
# starts from exactly this file: the SHA-256 in build/linux/bin/trespasser_recomp.build.json is checked first, the sources must still be
# the ones the binary was built from, and the headless self-check must still pass.
#
#   tools/package-linux.sh [--artifact FILE] [--out-dir DIR]
#
# Writes DIR (default dist/linux-staging) holding the binary, its manifest, trespasser_recomp.sha256, the launcher images the program reads
# beside itself (assets/launcher) and the AppRun, .desktop file and icon of the later AppImage step (packaging/linux). Local only.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
artifact=$repo/build/linux/bin/trespasser_recomp
out=$repo/dist/linux-staging
while [ $# -gt 0 ]; do
  case $1 in
    --artifact) artifact=$2; shift 2 ;;
    --out-dir) out=$2; shift 2 ;;
    -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "package-linux: unknown option $1" >&2; exit 2 ;;
  esac
done
[ -f "$artifact" ] || { echo "package-linux: $artifact does not exist: run tools\\test.ps1 first (it stages the validated binary)" >&2; exit 1; }
python3 -I "$repo/tools/linux_artifact.py" verify --elf "$artifact" --manifest "$(dirname "$artifact")/trespasser_recomp.build.json" || {
  echo "package-linux: the binary is not the validated one; nothing was packaged" >&2
  exit 1
}
app_id=$(sed -n 's/^#define JPT_APPLICATION_ID "\(.*\)"$/\1/p' "$repo/src/common/jpt_port.h")
desktop=$repo/packaging/linux/$app_id.desktop
if [ -z "$app_id" ] || [ ! -f "$desktop" ] || ! grep -qx "StartupWMClass=$app_id" "$desktop"; then
  echo "package-linux: the .desktop file in packaging/linux must be named $app_id.desktop and set StartupWMClass=$app_id (JPT_APPLICATION_ID in src/common/jpt_port.h)" >&2
  exit 1
fi
mkdir -p "$out/assets/launcher" "$out/packaging/linux"
cp -f -- "$artifact" "$out/trespasser_recomp.new"
cp -f -- "$(dirname "$artifact")/trespasser_recomp.build.json" "$out/"
mv -f -- "$out/trespasser_recomp.new" "$out/trespasser_recomp"
cp -f -- "$repo/assets/launcher/header-art.png" "$repo/assets/launcher/sponsor-heart.png" "$out/assets/launcher/"
cp -f -- "$repo/packaging/linux/AppRun" "$desktop" "$out/packaging/linux/"
cp -f -- "$repo/assets/app/trespasser-icon.png" "$out/packaging/linux/trespasser_recomp.png"
chmod +x "$out/packaging/linux/AppRun"
(cd "$out" && sha256sum trespasser_recomp > trespasser_recomp.sha256)
echo "package-linux: staged $out/trespasser_recomp ($(cut -d' ' -f1 "$out/trespasser_recomp.sha256"))"
