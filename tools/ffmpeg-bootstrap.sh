#!/usr/bin/env bash
# A pinned, LGPL-only FFmpeg for the tests of the Smacker backend, built once into a cache outside the repository:
#   tools/ffmpeg-bootstrap.sh [--cc COMPILER] [--prefix DIR] [--jobs N]
# Prints the prefix (include/ and lib/, what -DFFMPEG_ROOT takes) on its last line and exits 0. Exit 3 when the source cannot be
# had (no network and no JPT_FFMPEG_SOURCE), 4 when it was built but is not what is pinned, 2 for a missing tool. Nothing is
# installed on the system and no sudo is used; the build is reproduced only from the commit pinned here.
#
# FFmpeg n7.1.1, built static with libavformat, libavcodec and libavutil only and nothing but the Smacker demuxer, its two
# decoders and the two PCM decoders its audio tracks use: no --enable-gpl, no --enable-nonfree, no --enable-version3, no external library, no programs. JPT_FFMPEG_SOURCE is a
# checkout of that tag (its commit is verified) for machines without network; the cache directory is JPT_FFMPEG_CACHE.
set -uo pipefail

tag=n7.1.1
commit=db69d06eeeab4f46da15030a80d539efb4503ca8
url=https://github.com/FFmpeg/FFmpeg
flags_version=2
cc=${CC:-}
prefix=
jobs=$(nproc 2>/dev/null || echo 2)
while [ $# -gt 0 ]; do
  case $1 in
    --cc) cc=$2; shift 2 ;;
    --prefix) prefix=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    *) echo "ffmpeg-bootstrap: unknown option $1" >&2; exit 2 ;;
  esac
done
[ -n "$cc" ] || for candidate in gcc gcc-13 gcc-12 gcc-11 clang; do command -v "$candidate" >/dev/null 2>&1 && { cc=$candidate; break; }; done
[ -n "$cc" ] || { echo "ffmpeg-bootstrap: no C compiler" >&2; exit 2; }
for tool in git make; do command -v "$tool" >/dev/null 2>&1 || { echo "ffmpeg-bootstrap: $tool is not installed" >&2; exit 2; }; done

cache=${JPT_FFMPEG_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/jpt-recomp/ffmpeg}
[ -n "$prefix" ] || prefix=$cache/$tag-${commit:0:8}-lgpl-smacker
configure_flags=(
  --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-debug --disable-network
  --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample --disable-postproc
  --disable-x86asm --disable-asm
  --enable-static --disable-shared --enable-pic
  --enable-avformat --enable-avcodec --enable-avutil
  --enable-protocol=file --enable-demuxer=smacker --enable-decoder=smacker --enable-decoder=smackaud
  --enable-decoder=pcm_s16le --enable-decoder=pcm_u8
)
stamp="$tag $commit flags=$flags_version $(printf '%s ' "${configure_flags[@]}")"

if [ -f "$prefix/.jpt-ffmpeg-stamp" ] && [ "$(cat "$prefix/.jpt-ffmpeg-stamp")" = "$stamp" ] && [ -f "$prefix/lib/libavformat.a" ] &&
   [ -f "$prefix/lib/libavcodec.a" ] && [ -f "$prefix/lib/libavutil.a" ]; then
  echo "$prefix"
  exit 0
fi

mkdir -p "$cache" || exit 2
src=$cache/src-$tag-${commit:0:8}
if [ -n "${JPT_FFMPEG_SOURCE:-}" ]; then
  src=$JPT_FFMPEG_SOURCE
elif [ ! -d "$src/.git" ]; then
  rm -rf "$src"
  if ! timeout 600 git clone --quiet --depth 1 --branch "$tag" "$url" "$src" 2>/dev/null; then
    echo "ffmpeg-bootstrap: cannot fetch $url $tag (offline?): set JPT_FFMPEG_SOURCE to a checkout of $tag" >&2
    rm -rf "$src"
    exit 3
  fi
fi
head=$(git -C "$src" rev-parse HEAD 2>/dev/null)
if [ "$head" != "$commit" ]; then
  echo "ffmpeg-bootstrap: $src is at ${head:-no commit}, not the pinned $tag ($commit)" >&2
  exit 4
fi

build=$cache/build-$tag-${commit:0:8}
rm -rf "$build" "$prefix" "$prefix.stage"
mkdir -p "$build" || exit 2
log=$build/bootstrap.log
# The prefix the libraries record is neutral and the source is reached by a relative path, so no path of this machine ends up in the
# executable; the files are staged and moved.
relative_src=$(realpath --relative-to="$build" "$src")
if ! (cd "$build" && "$relative_src/configure" --prefix=/jpt-ffmpeg --cc="$cc" "${configure_flags[@]}" >"$log" 2>&1 &&
      make -j"$jobs" >>"$log" 2>&1 && make install DESTDIR="$prefix.stage" >>"$log" 2>&1 &&
      mv "$prefix.stage/jpt-ffmpeg" "$prefix" && rm -rf "$prefix.stage"); then
  echo "ffmpeg-bootstrap: the build failed, see $log" >&2
  tail -20 "$log" >&2
  exit 4
fi
config=$(grep -h "^#define FFMPEG_CONFIGURATION" "$prefix/include/libavutil/ffversion.h" "$build/config.h" 2>/dev/null | head -1)
if grep -qE -- "--enable-(gpl|nonfree|version3)" <<<"$config"; then
  echo "ffmpeg-bootstrap: the build is not LGPL-only: $config" >&2
  exit 4
fi
printf '%s' "$stamp" >"$prefix/.jpt-ffmpeg-stamp"
echo "$prefix"
