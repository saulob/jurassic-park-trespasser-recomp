#!/usr/bin/env bash
# A pinned, static OpenSSL for the Bug Report's HTTPS, built once into a cache outside the repository:
#   tools/openssl-bootstrap.sh [--cc COMPILER] [--prefix DIR] [--jobs N]
# Prints the prefix (include/ and lib/, what -DOPENSSL_ROOT_DIR takes) on its last line and exits 0. Exit 3 when the source cannot
# be had (no network and no JPT_OPENSSL_TARBALL or JPT_OPENSSL_SOURCE), 4 when it was built but is not what is pinned, 2 for a
# missing tool. Nothing is installed on the system and no sudo is used; the tarball is the pinned release, checked by its SHA-256.
set -uo pipefail

version=3.5.9
sha256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
url="https://github.com/openssl/openssl/releases/download/openssl-$version/openssl-$version.tar.gz"
flags_version=1
cc=
prefix=
jobs=$(nproc 2>/dev/null || echo 2)
while [ $# -gt 0 ]; do
  case $1 in
    --cc) cc=$2; shift 2 ;;
    --prefix) prefix=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    *) echo "openssl-bootstrap: unknown option $1" >&2; exit 2 ;;
  esac
done
[ -n "$cc" ] || for candidate in gcc gcc-13 gcc-12 gcc-11 clang; do command -v "$candidate" >/dev/null 2>&1 && { cc=$candidate; break; }; done
[ -n "$cc" ] || { echo "openssl-bootstrap: no C compiler" >&2; exit 2; }
for tool in perl make tar; do command -v "$tool" >/dev/null 2>&1 || { echo "openssl-bootstrap: $tool is not installed" >&2; exit 2; }; done
case $cc in
  clang*) cxx=${cc/clang/clang++} ;;
  gcc*) cxx=${cc/gcc/g++} ;;
  *) cxx=c++ ;;
esac
command -v "$cxx" >/dev/null 2>&1 || { echo "openssl-bootstrap: $cxx is not installed" >&2; exit 2; }

cache=${JPT_OPENSSL_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/jpt-recomp/openssl}
[ -n "$prefix" ] || prefix=$cache/openssl-$version-static
configure_flags=(no-shared no-tests no-docs)
stamp="$version $sha256 flags=$flags_version $cc $(printf '%s ' "${configure_flags[@]}")"

if [ -f "$prefix/.jpt-openssl-stamp" ] && [ "$(cat "$prefix/.jpt-openssl-stamp")" = "$stamp" ] &&
   [ -f "$prefix/lib/libssl.a" ] && [ -f "$prefix/lib/libcrypto.a" ] && [ -f "$prefix/include/openssl/ssl.h" ]; then
  echo "$prefix"
  exit 0
fi

mkdir -p "$cache" || exit 2
src=$cache/src-$version
if [ -n "${JPT_OPENSSL_SOURCE:-}" ]; then
  src=$JPT_OPENSSL_SOURCE
elif [ ! -f "$src/Configure" ]; then
  tarball=${JPT_OPENSSL_TARBALL:-$cache/openssl-$version.tar.gz}
  if [ ! -f "$tarball" ]; then
    if ! command -v curl >/dev/null 2>&1; then
      echo "openssl-bootstrap: curl is not installed and JPT_OPENSSL_TARBALL is not set" >&2
      exit 3
    fi
    if ! timeout 600 curl -fsSL --retry 3 -o "$tarball.part" "$url"; then
      echo "openssl-bootstrap: cannot fetch $url (offline?): set JPT_OPENSSL_TARBALL or JPT_OPENSSL_SOURCE" >&2
      rm -f "$tarball.part"
      exit 3
    fi
    mv "$tarball.part" "$tarball"
  fi
  if [ "$(sha256sum "$tarball" | awk '{print $1}')" != "$sha256" ]; then
    echo "openssl-bootstrap: $tarball is not OpenSSL $version ($sha256)" >&2
    exit 4
  fi
  rm -rf "$src" && mkdir -p "$src" && tar -xzf "$tarball" -C "$src" --strip-components=1 || exit 4
fi

build=$cache/build-$version-static
rm -rf "$build" "$prefix" "$prefix.stage"
mkdir -p "$build" || exit 2
log=$build/bootstrap.log
# The prefix the libraries record is neutral and the files are staged, so no path of this machine ends up in the executable.
if ! (cd "$build" && CC="$cc" CXX="$cxx" "$src/Configure" --prefix=/jpt-openssl --libdir=lib "${configure_flags[@]}" >"$log" 2>&1 &&
      make -j"$jobs" build_libs >>"$log" 2>&1 && make install_dev DESTDIR="$prefix.stage" >>"$log" 2>&1 &&
      mv "$prefix.stage/jpt-openssl" "$prefix" && rm -rf "$prefix.stage"); then
  echo "openssl-bootstrap: the build failed, see $log" >&2
  tail -20 "$log" >&2
  exit 4
fi
printf '%s' "$stamp" >"$prefix/.jpt-openssl-stamp"
echo "$prefix"
