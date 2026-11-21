#!/usr/bin/env bash
# Takes the Linux executable the gate validated and prepares it for packaging; it never builds. Packaging (AppImage, Steam Deck) later
# starts from exactly this file: the SHA-256 in build/linux/bin/trespasser_recomp.build.json is checked first, the sources must still be
# the ones the binary was built from, and the headless self-check must still pass.
#
#   tools/package-linux.sh [--artifact FILE] [--out-dir DIR]
#
# Writes DIR (default dist/linux-staging) holding the binary, its manifest and trespasser_recomp.sha256. Local only.
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
mkdir -p "$out"
cp -f -- "$artifact" "$out/trespasser_recomp.new"
cp -f -- "$(dirname "$artifact")/trespasser_recomp.build.json" "$out/"
mv -f -- "$out/trespasser_recomp.new" "$out/trespasser_recomp"
(cd "$out" && sha256sum trespasser_recomp > trespasser_recomp.sha256)
echo "package-linux: staged $out/trespasser_recomp ($(cut -d' ' -f1 "$out/trespasser_recomp.sha256"))"
