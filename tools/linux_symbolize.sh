#!/usr/bin/env bash
# Symbolizes the host backtrace of a crash report (the lines trespasser_recomp writes to the log and stderr on a fatal signal).
#
#   tools/linux_symbolize.sh [--binary build/linux/bin/trespasser_recomp] crash_log_or_-
#
# Report lines look like  .../trespasser_recomp(+0x1a2b3c) [0x5555...]  or  [0x5555...]; the load base is the
# "load base 0x..." line. Needs addr2line (binutils) and a build with debug info (RelWithDebInfo, the default).
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
binary=$repo/build/linux/bin/trespasser_recomp
if [ "${1:-}" = "--binary" ]; then binary=$2; shift 2; fi
input=${1:--}

command -v addr2line >/dev/null 2>&1 || { echo "linux_symbolize: addr2line is not installed (binutils)" >&2; exit 2; }
text=$(cat "$input")
base=$(printf '%s\n' "$text" | sed -n 's/.*load base *\(0x[0-9a-f]*\).*/\1/p' | head -1)
[ -n "$base" ] || { echo "linux_symbolize: no 'load base' line in the report" >&2; exit 2; }

printf '%s\n' "$text" | grep -o '\[0x[0-9a-f]*\]' | tr -d '[]' | while read -r addr; do
  offset=$(( addr - base ))
  if [ "$offset" -lt 0 ]; then
    printf '%s  (outside the executable)\n' "$addr"
  else
    printf '%s  ' "$addr"
    addr2line -f -C -i -e "$binary" "$(printf '0x%x' "$offset")" | paste -sd' ' -
  fi
done
