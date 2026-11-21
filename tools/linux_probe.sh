#!/usr/bin/env bash
# What a WSL distribution offers the Linux gate: one key=value per line. Read-only; nothing is installed or started.
glibc=$(ldd --version 2>&1 | head -1 | awk '{print $NF}')
echo "glibc=$glibc"
for tool in cmake ninja python3 pkg-config; do
  echo "$tool=$(command -v "$tool" || true)"
done
for compiler in gcc gcc-13 gcc-12 gcc-11 clang; do
  if command -v "$compiler" >/dev/null 2>&1; then
    echo "cc=$compiler"
    break
  fi
done
echo "os=$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
if pkg-config --exists libavformat libavcodec libavutil 2>/dev/null; then echo "ffmpeg=yes"; else echo "ffmpeg=no"; fi
if [ -f /usr/include/X11/Xlib.h ]; then echo "x11=yes"; else echo "x11=no"; fi
