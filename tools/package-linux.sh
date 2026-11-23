#!/usr/bin/env bash
# Packages the validated Linux Release build as dist/<asset>-v<version>-<suffix>.AppImage and its .sha256. Local
# only: nothing is published, tagged or uploaded. The Linux counterpart of tools\package.ps1.
#
#   tools/package-linux.sh                  build the Linux Release gate, then package
#   tools/package-linux.sh --skip-build     package the existing canonical Release artifact
#   tools/package-linux.sh --clean          remove the Release build first
#   tools/package-linux.sh --out-dir DIR    write the AppImage, its .sha256 and the symbols to DIR instead of dist/
#   tools/package-linux.sh --verify DIR     only check an existing AppDir
#   tools/package-linux.sh --verify-only F  only check a finished AppImage (runtime, then its extracted contents)
#   tools/package-linux.sh --self-test      run the release checks on fixtures that must pass and must fail
#
# The version comes from project VERSION in CMakeLists.txt; the asset prefix and suffix come from config/release.json.
# The canonical artifact is build/linux/bin/trespasser_recomp with its build manifest; an ELF
# from build/linux-work or any other path is never packaged. Build on Ubuntu 20.04 (packaging/linux/ci-deps.sh): no
# shipped ELF may need a newer glibc than 2.31. GPU drivers, the Vulkan loader, X11/Wayland, the audio servers and
# glibc come from the player's system. The patched AppImage runtime is built by tools/build-appimage-runtime.sh,
# which needs root or passwordless sudo (docs/LINUX.md).
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
skip_build=0 clean=0 verify_dir= verify_file= out_dir= self_test=0
while (($#)); do
    case $1 in
        --skip-build) skip_build=1 ;;
        --clean) clean=1 ;;
        --out-dir) out_dir=${2:?--out-dir needs a folder}; shift ;;
        --verify) verify_dir=$(cd -- "${2:?--verify needs a folder}" && pwd) || exit 2; shift ;;
        --verify-only) verify_file=$(realpath -- "${2:?--verify-only needs an AppImage}") || exit 2; shift ;;
        --self-test) self_test=1 ;;
        -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
if [[ -n $out_dir ]]; then
    mkdir -p -- "$out_dir" && out_dir=$(cd -- "$out_dir" && pwd) || exit 2
fi
cd "$root"

step() { printf '\n==> %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

max_glibc=2.31
program=trespasser_recomp
appimagetool_version=1.9.1
appimagetool_sha256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

# Provided by the player's system: glibc, the Vulkan loader and drivers, the display servers, the audio servers and
# the system services SDL talks to. Nothing here may be bundled.
host_libs='^(ld-linux.*|libc|libm|libdl|librt|libpthread|libresolv|libutil|libnsl|libanl|libcrypt|libmvec|libvulkan|libGL|libGLX|libGLdispatch|libEGL|libOpenGL|libgbm|libdrm.*|libX11|libX11-xcb|libxcb.*|libXau|libXdmcp|libXext|libXfixes|libXrender|libXi|libXcursor|libXrandr|libXss|libXtst|libXinerama|libxkbcommon.*|libwayland-.*|libdecor-.*|libasound|libudev|libpulse.*|libpipewire.*|libjack|libsndio|libdbus-1|libibus-1\.0|libfontconfig|libOpenCL|libva.*|libvdpau|libnvidia-.*|libcuda|libz|libselinux|libpcre2-.*|libsystemd|libcap|libusb-1\.0)\.so'
# These are compiled into the executable in the Release build. Finding them in usr/lib means the build changed.
static_only='^(libstdc\+\+|libgcc_s|libssl|libcrypto|libav(codec|format|util)|libSDL3)\.so'

# Matched without case: game data, user data and build output never ship, whatever their spelling.
forbidden_names='^(game|user|user_data|user_data\.migrating|saves|cache|logs|downloads|staging|mods|build|dist|out|prepared|extracted|image|symbols|renderdoc.*|librenderdoc.*|vklayer_.*|implicit_layer\.d|explicit_layer\.d|icd\.d|trespasser\.ini|recomp-registry\.txt|savegame\..*|cmakefiles|__pycache__)$'
forbidden_patterns='\.(pkg|pdb|debug|log|dmp|ini|bin|dat|self|sprx|prx|elf|cpk|sfo|json|trp|spv|pipeline_cache|o|obj|a|lib|tmp|bak|part|sav|save|undo|py|pyc|ps1|c|cpp|h|hpp|env|pem|key|iso|tpa|smk|wtd|scn|grf|swp|dll|exe)$'
glibc_parts='^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libmvec|libanl|libnsl|libBrokenLocale|libthread_db|libnss_.*)\.so'

# Text that must not appear in any shipped file: build machine paths, private tooling and temporary markers.
fixed_needles=(VisualStudio DEBUG_SAULO build/cmake build/linux-work jpt-recomp/openssl jpt-recomp/ffmpeg
               Decomp_Downloads Trespasser_English_captions referencias-ps4)

newer_than() { [[ $1 != "$2" && $(printf '%s\n' "$1" "$2" | sort -V | tail -1) == "$1" ]]; }
magic_of() { head -c 4 -- "$1" | od -An -tx1 | tr -d ' \n'; }
is_elf() { [[ $(magic_of "$1") == 7f454c46 ]]; }
# The newest GLIBC_ version in `readelf --version-info` text: the dynamic loader refuses to start an ELF without it.
newest_glibc_in() {
    awk '/^Version needs section/ {needs = 1; next} /^Version / {needs = 0} needs' |
        { grep -oE 'Name: GLIBC_[0-9]+(\.[0-9]+)*' || true; } | sed 's/^Name: GLIBC_//' | sort -V | tail -1
}
# `readelf -d` text: one "KIND value" line per DT_NEEDED, DT_RPATH and DT_RUNPATH entry.
dynamic_entries_in() {
    sed -nE 's/.*\((NEEDED|RPATH|RUNPATH)\)[^[]*\[([^]]*)\].*/\1 \2/p'
}
# The bytes of a text as UTF-16LE, as a grep -P pattern.
utf16_pattern() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n' | sed 's/../\\x&\\x00/g'; }

# The rules for one ELF, given its readelf output: glibc baseline, the C++ runtime, debug sections, RPATH/RUNPATH
# and the libraries it needs.
check_elf_text() { # label relative-path version-info-text dynamic-text bundled-names sections-text
    local label=$1 rel=$2 versions=$3 dynamic=$4 bundled=$5 sections=$6 newest kind value expected_rpath saw_rpath=0 cxx
    newest=$(newest_glibc_in <<<"$versions")
    printf '  %-48s glibc %s\n' "$rel" "${newest:-(none)}"
    if [[ -n $newest ]]; then
        if [[ -z $glibc_newest ]] || newer_than "$newest" "$glibc_newest"; then glibc_newest=$newest glibc_newest_file=$rel; fi
        newer_than "$newest" "$max_glibc" &&
            problem "$label $rel needs glibc $newest, newer than the $max_glibc baseline (build on Ubuntu 20.04)"
    fi
    cxx=$(grep -oE 'Name: (GLIBCXX|CXXABI|GCC)_[0-9.]+' <<<"$versions" | sort -V | tail -1 || true)
    [[ -z $cxx ]] || problem "$label $rel links the C++ runtime dynamically (needs $cxx): the Release build is static"
    if [[ -n $sections ]] && grep -qE '\s\.(z?debug_[a-z_]+|gnu_debugdata)\s' <<<"$sections"; then
        problem "debug sections in $label $rel"
    fi
    case $rel in
        usr/bin/$program) expected_rpath='RPATH $ORIGIN/../lib' ;;
        usr/lib/*) expected_rpath='RPATH $ORIGIN' ;;
        *) expected_rpath= ;;
    esac
    while read -r kind value; do
        [[ -n ${kind:-} ]] || continue
        case $kind in
            NEEDED)
                if ! grep -qxF -- "$value" <<<"$bundled" && ! [[ $value =~ $host_libs ]]; then
                    problem "$label $rel needs $value, which is neither bundled nor a permitted host library"
                fi ;;
            RPATH|RUNPATH)
                saw_rpath=1
                [[ "$kind $value" == "$expected_rpath" ]] ||
                    problem "$label $rel has $kind '$value', expected ${expected_rpath:-no RPATH/RUNPATH}"
                ;;
        esac
    done < <(dynamic_entries_in <<<"$dynamic")
    [[ -z $expected_rpath || $saw_rpath == 1 ]] ||
        problem "$label $rel has no RPATH: it would not find its libraries"
}

# The only files the AppDir may hold. Every directory is listed too, so an unexpected tree is refused.
expected_entry() { # relative path
    case $1 in
        AppRun|"$desktop_file"|"$app_icon"|.DirIcon|usr|usr/bin|usr/bin/"$program"|usr/bin/assets|usr/bin/assets/launcher|\
        usr/bin/assets/launcher/header-art.png|usr/bin/assets/launcher/sponsor-heart.png|usr/lib|usr/lib/compat|\
        usr/lib/*.so*|usr/lib/compat/*.so*|usr/share|usr/share/applications|usr/share/applications/"$desktop_file"|\
        usr/share/metainfo|usr/share/metainfo/"$app_id.appdata.xml"|usr/share/icons|usr/share/icons/hicolor|\
        usr/share/icons/hicolor/256x256|usr/share/icons/hicolor/256x256/apps|\
        usr/share/icons/hicolor/256x256/apps/"$app_icon"|usr/share/licenses|usr/share/licenses/"$doc_name"|\
        usr/share/licenses/"$doc_name"/*|usr/share/doc|usr/share/doc/"$doc_name"|usr/share/doc/"$doc_name"/*) return 0 ;;
    esac
    return 1
}

required_leaves() {
    printf '%s\n' \
        AppRun "$desktop_file" "$app_icon" "usr/share/applications/$desktop_file" "$metainfo" \
        "usr/share/icons/hicolor/256x256/apps/$app_icon" "usr/bin/$program" \
        usr/bin/assets/launcher/header-art.png usr/bin/assets/launcher/sponsor-heart.png \
        "usr/share/licenses/$doc_name/LICENSE.txt" "usr/share/licenses/$doc_name/LEGAL.txt" \
        "usr/share/licenses/$doc_name/THIRD-PARTY-LICENSES.txt" \
        "usr/share/licenses/$doc_name/FFmpeg-LGPL-2.1.txt" "usr/share/licenses/$doc_name/FFMPEG-SOURCE.txt" \
        "usr/share/licenses/$doc_name/OpenSSL-LICENSE.txt" "usr/share/licenses/$doc_name/Apache-2.0.txt" \
        "usr/share/doc/$doc_name/README.txt"
}

check_appdir() { # dir [the AppImage file, whose runtime is an ELF too]
    local dir=$1 problems=0 item rel name lower needle magic bundled='' i patterns=() needles=() elves=()
    glibc_newest= glibc_newest_file=
    problem() { echo "  $*" >&2; problems=$((problems + 1)); }
    needles=("$root" "${HOME:-/nonexistent-home}" "${fixed_needles[@]}")
    for needle in "${needles[@]}"; do patterns+=("$(utf16_pattern "$needle")"); done
    while IFS= read -r -d '' item; do
        rel=${item#"$dir"/}
        name=$(basename -- "$item")
        lower=${name,,}
        expected_entry "$rel" || problem "unexpected item: $rel"
        [[ $lower =~ $forbidden_names ]] && problem "forbidden name: $rel"
        [[ $name =~ $glibc_parts ]] && problem "glibc must come from the player's system: $rel"
        [[ $rel == usr/lib/* && $name =~ $host_libs ]] && problem "a host library must come from the player's system: $rel"
        [[ $rel == usr/lib/* && $name =~ $static_only ]] &&
            problem "a library linked statically in the Release build must not be staged: $rel"
        [[ $lower == *renderdoc* || $lower == *vklayer* || $name == *_layer.d ]] &&
            problem "RenderDoc or Vulkan layer file: $rel"
        if [[ $rel != usr/share/licenses/* && $rel != usr/share/doc/* && -f $item && $lower =~ $forbidden_patterns ]]; then
            problem "forbidden file type: $rel"
        fi
        [[ -L $item && $rel != .DirIcon ]] && problem "symbolic link: $rel"
        [[ $rel == .DirIcon && ! -L $item ]] && problem ".DirIcon is not a symbolic link"
        [[ $rel == .DirIcon && -L $item && $(readlink -- "$item") != "$app_icon" ]] &&
            problem ".DirIcon does not point to $app_icon"
        if [[ -f $item && ! -L $item ]]; then
            magic=$(magic_of "$item")
            case $magic in
                7f434e54) problem "PS4 package: $rel" ;;
                4f153d1d) problem "PS4 SELF: $rel" ;;
                43504b20) problem "CPK archive: $rel" ;;
                4d5a*) problem "Windows program: $rel" ;;
                7f454c46) elves+=("$item") ;;
            esac
            for needle in "${needles[@]}"; do
                grep -a -q -i -F -- "$needle" "$item" &&
                    { problem "local path, development marker or stale name '$needle' in $rel"; break; }
            done
            for i in "${!patterns[@]}"; do
                LC_ALL=C grep -a -q -P -- "${patterns[$i]}" "$item" &&
                    { problem "UTF-16 local path or marker '${needles[$i]}' in $rel"; break; }
            done
        fi
    done < <(find "$dir" -mindepth 1 -print0)
    while IFS= read -r leaf; do
        [[ -e $dir/$leaf || -L $dir/$leaf ]] || problem "missing: $leaf"
    done < <(required_leaves)
    [[ -e $dir/.DirIcon || -L $dir/.DirIcon ]] || problem "missing: .DirIcon"
    for leaf in AppRun "usr/bin/$program"; do
        [[ ! -e $dir/$leaf || -x $dir/$leaf ]] || problem "not executable: $leaf"
    done
    if [[ -d $dir/usr/lib ]]; then
        bundled=$(cd "$dir/usr/lib" && { ls -1 -- *.so* compat/*.so* 2>/dev/null || true; } | sed 's|.*/||' | sort -u)
    fi
    if [[ -f $dir/usr/bin/$program && -x $dir/usr/bin/$program ]] && is_elf "$dir/usr/bin/$program"; then
        local missing
        missing=$(env -u LD_LIBRARY_PATH ldd "$dir/usr/bin/$program" 2>&1 | grep 'not found' || true)
        [[ -z $missing ]] || problem "unresolved libraries: $missing"
    fi
    echo "Each ELF: newest glibc import (baseline $max_glibc), RPATH/RUNPATH, needed libraries:"
    local file
    for file in "${elves[@]}"; do
        check_elf_text 'the AppDir:' "${file#"$dir"/}" "$(readelf --version-info -W -- "$file" 2>/dev/null)" \
            "$(readelf -d -W -- "$file" 2>/dev/null)" "$bundled" "$(readelf -S -W -- "$file" 2>/dev/null)"
    done
    if (($# >= 2)); then
        check_elf_text 'the AppImage runtime:' "$(basename -- "$2")" "$(readelf --version-info -W -- "$2" 2>/dev/null)" \
            "$(readelf -d -W -- "$2" 2>/dev/null)" '' ''
    fi
    echo "Newest glibc import: ${glibc_newest:-(none)}${glibc_newest_file:+ ($glibc_newest_file)}; baseline $max_glibc"
    ((problems == 0)) || die "release check failed ($problems problem(s))"
    echo 'AppDir is clean'
}

# The runtime at the front of the AppImage is the reviewed one: its bytes up to the file system equal the runtime
# file, apart from the sections appimagetool fills (digest, update information, signature).
verify_embedded_runtime() { # appimage runtime
    local appimage=$1 runtime=$2 size offset work section start length masked=0
    size=$(stat -c %s -- "$runtime")
    offset=$("$appimage" --appimage-offset)
    [[ $offset == "$size" ]] || die "the AppImage's file system starts at $offset, the pinned runtime is $size bytes"
    work=$(mktemp -d)
    head -c "$size" -- "$appimage" > "$work/embedded"
    cp -- "$runtime" "$work/pinned"
    # readelf -S -W: name, type, address, then the file offset and size in hexadecimal.
    while read -r section start length; do
        for copy in embedded pinned; do
            dd if=/dev/zero of="$work/$copy" bs=1 seek=$((16#$start)) count=$((16#$length)) conv=notrunc status=none
        done
        masked=$((masked + 1))
    done < <(readelf -S -W -- "$work/pinned" | awk '{for (i = 1; i < NF - 3; i++) if ($i == ".digest_md5" || $i == ".upd_info" ||
        $i == ".sha256_sig" || $i == ".sig_key") print $i, $(i + 3), $(i + 4)}')
    ((masked > 0)) || { rm -rf -- "$work"; die 'the pinned runtime has none of the sections appimagetool fills'; }
    cmp -s -- "$work/embedded" "$work/pinned" || { rm -rf -- "$work"; die 'the runtime embedded in the AppImage is not the pinned runtime'; }
    rm -rf -- "$work"
    echo "Embedded runtime: the pinned runtime ($(sha256sum -- "$runtime" | cut -d' ' -f1)), $size bytes"
}

run_self_test() {
    local work failures=0 output
    work=$(mktemp -d)
    trap 'rm -rf -- "$work"' RETURN
    make_fixture() {
        rm -rf -- "$work/app"
        local leaf
        while IFS= read -r leaf; do
            mkdir -p "$work/app/$(dirname -- "$leaf")"
            printf 'fixture\n' > "$work/app/$leaf"
        done < <(required_leaves)
        mkdir -p "$work/app/usr/lib"
        chmod 755 "$work/app/AppRun" "$work/app/usr/bin/$program"
        rm -f "$work/app/.DirIcon"
        ln -s "$app_icon" "$work/app/.DirIcon"
    }
    expect() { # pass|fail description command...
        local want=$1 what=$2 status=0
        shift 2
        output=$( ("$@") 2>&1 ) || status=$?
        if [[ $want == pass && $status != 0 ]] || [[ $want == fail && $status == 0 ]]; then
            echo "SELF-TEST FAIL: $what (expected $want)"; printf '%s\n' "$output" | sed 's/^/    /'
            failures=$((failures + 1))
        else
            echo "ok: $what"
        fi
    }
    make_fixture
    expect pass 'a clean AppDir' check_appdir "$work/app"
    make_fixture; mkdir "$work/app/usr/bin/game"
    expect fail 'a game folder' check_appdir "$work/app"
    make_fixture; echo x > "$work/app/usr/bin/ROBO_US.CPK"
    expect fail 'an upper-case .CPK archive' check_appdir "$work/app"
    make_fixture; echo x > "$work/app/usr/bin/EBOOT.BIN"
    expect fail 'an eboot.bin in any case' check_appdir "$work/app"
    make_fixture; printf '\x7fCNT' > "$work/app/$app_icon"
    expect fail 'a PS4 package by content' check_appdir "$work/app"
    make_fixture; printf 'CPK ' > "$work/app/usr/bin/assets/launcher/sponsor-heart.png"
    expect fail 'a CPK archive by content' check_appdir "$work/app"
    make_fixture; echo "$root/build/cmake/linux" > "$work/app/usr/share/doc/$doc_name/README.txt"
    expect fail 'a local build path' check_appdir "$work/app"
    make_fixture; printf 'V\0i\0s\0u\0a\0l\0S\0t\0u\0d\0i\0o\0\n' > "$work/app/usr/share/doc/$doc_name/README.txt"
    expect fail 'a UTF-16 local path' check_appdir "$work/app"
    make_fixture; echo 'DEBUG_SAULO probe' > "$work/app/AppRun"
    expect fail 'a DEBUG_SAULO marker' check_appdir "$work/app"
    make_fixture; echo x > "$work/app/usr/lib/libvulkan.so.1"
    expect fail 'a bundled Vulkan loader' check_appdir "$work/app"
    make_fixture; echo x > "$work/app/usr/lib/libc.so.6"
    expect fail 'a bundled glibc' check_appdir "$work/app"
    make_fixture; echo x > "$work/app/usr/lib/libavcodec.so.61"
    expect fail 'a bundled FFmpeg library' check_appdir "$work/app"
    make_fixture; echo x > "$work/app/usr/lib/libssl.so.3"
    expect fail 'a bundled OpenSSL library' check_appdir "$work/app"
    make_fixture; echo x > "$work/app/usr/lib/libSDL3.so.0"
    expect fail 'a bundled SDL3 library' check_appdir "$work/app"
    make_fixture; echo x > "$work/app/usr/lib/libstdc++.so.6"
    expect fail 'a dynamic C++ runtime' check_appdir "$work/app"
    make_fixture; echo x > "$work/app/usr/lib/librenderdoc.so"
    expect fail 'RenderDoc' check_appdir "$work/app"
    make_fixture; mkdir -p "$work/app/usr/share/vulkan/implicit_layer.d"
    expect fail 'a Vulkan layer manifest folder' check_appdir "$work/app"
    make_fixture; echo x > "$work/app/usr/bin/tool"
    expect fail 'an unexpected program' check_appdir "$work/app"
    make_fixture; rm "$work/app/usr/share/licenses/$doc_name/FFmpeg-LGPL-2.1.txt"
    expect fail 'a missing FFmpeg licence' check_appdir "$work/app"
    make_fixture; rm "$work/app/$app_icon"
    expect fail 'a missing icon' check_appdir "$work/app"
    make_fixture; rm "$work/app/.DirIcon"; printf 'not a link\n' > "$work/app/.DirIcon"
    expect fail 'a regular .DirIcon' check_appdir "$work/app"
    make_fixture; rm "$work/app/.DirIcon"; ln -s other.png "$work/app/.DirIcon"
    expect fail 'a wrong .DirIcon target' check_appdir "$work/app"

    local needs_230='Version needs section '"'"'.gnu.version_r'"'"' contains 1 entry:
  000000: Version: 1  File: libc.so.6  Cnt: 2
  0x0010:   Name: GLIBC_2.30  Flags: none  Version: 3'
    local needs_234=${needs_230//GLIBC_2.30/GLIBC_2.34}
    local needs_cxx="$needs_230"$'\n'"  0x0020:   Name: GLIBCXX_3.4.30  Flags: none  Version: 4"
    local good_dynamic=' 0x0000000000000001 (NEEDED)             Shared library: [libfoo.so.1]
 0x0000000000000001 (NEEDED)             Shared library: [libvulkan.so.1]
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
 0x000000000000000f (RPATH)              Library rpath: [$ORIGIN/../lib]'
    local bundled=$'libfoo.so.1\nlibbar.so.1'
    elf_case() { # versions dynamic relative-path [bundled]
        glibc_newest= glibc_newest_file=
        local problems=0
        problem() { echo "  $*" >&2; problems=$((problems + 1)); }
        check_elf_text 'fixture:' "${3:-usr/bin/$program}" "$1" "$2" "${4:-$bundled}" "${5:-}"
        ((problems == 0))
    }
    expect pass 'glibc 2.30, bundled and host libraries, $ORIGIN RPATH' elf_case "$needs_230" "$good_dynamic"
    expect fail 'an ELF that needs glibc 2.34' elf_case "$needs_234" "$good_dynamic"
    expect fail 'an absolute RPATH' elf_case "$needs_230" "${good_dynamic//\$ORIGIN\/..\/lib//root/.cache/x}"
    expect fail 'a RUNPATH instead of the RPATH' elf_case "$needs_230" "${good_dynamic//(RPATH)              Library rpath/(RUNPATH)            Library runpath}"
    expect fail 'no RPATH on the executable' elf_case "$needs_230" "$(grep -v RPATH <<<"$good_dynamic")"
    expect fail 'a library that is neither bundled nor permitted' elf_case "$needs_230" "$good_dynamic
 0x0000000000000001 (NEEDED)             Shared library: [libavcodec.so.61]"
    expect fail 'debug sections' elf_case "$needs_230" "$good_dynamic" usr/bin/$program "$bundled" '  [ 1] .debug_info PROGBITS'
    expect fail 'a dynamic C++ runtime' elf_case "$needs_cxx" "$good_dynamic"
    expect pass 'a bundled library with $ORIGIN' elf_case "$needs_230" ' 0x1 (NEEDED) Shared library: [libc.so.6]
 0x1d (RPATH) Library rpath: [$ORIGIN]' usr/lib/libfoo.so.1
    expect fail 'a bundled library with a build path' elf_case "$needs_230" ' 0x1d (RPATH) Library rpath: [/usr/local/lib]' \
        usr/lib/libbar.so.1
    ((failures == 0)) || die "package-linux.sh self-test: $failures case(s) failed"
    echo 'package-linux.sh self-test: every case passed'
}

version=$(sed -nE 's/^project\(trespasser_recomp VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt)
[[ -n $version ]] || die 'project VERSION not found in CMakeLists.txt'
asset_prefix=$(sed -n 's/.*"asset_prefix"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' config/release.json | head -1)
linux_suffix=$(sed -n 's/.*"linux_suffix"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' config/release.json | head -1)
[[ -n $asset_prefix && -n $linux_suffix ]] || die 'asset_prefix or linux_suffix not found in config/release.json'
app_id=$(sed -n 's/^#define JPT_APPLICATION_ID "\(.*\)"$/\1/p' src/common/jpt_port.h)
[[ -n $app_id ]] || die 'JPT_APPLICATION_ID not found in src/common/jpt_port.h'
desktop_file=$app_id.desktop
appdata_file=$app_id.appdata.xml
app_icon=$app_id.png
metainfo=usr/share/metainfo/$appdata_file
doc_name=$asset_prefix
name=$asset_prefix-v$version-$linux_suffix
artifact=$root/build/linux/bin/$program
manifest=$root/build/linux/bin/$program.build.json
build_dir=$root/build/linux-release
dist=${out_dir:-$root/dist}
work=$root/build/appimage
appdir=$work/AppDir
symbols=$dist/$name-symbols

if ((self_test)); then
    run_self_test
    exit 0
fi
if [[ -n $verify_dir ]]; then
    check_appdir "$verify_dir"
    exit 0
fi
if [[ -n $verify_file ]]; then
    [[ -f $verify_file ]] || die "Missing $verify_file"
    bash "$root/tools/build-appimage-runtime.sh" --check "$verify_file"
    verify_work=$(mktemp -d)
    trap 'rm -rf "$verify_work"' EXIT
    cp -- "$verify_file" "$verify_work/verify.AppImage"
    chmod 755 "$verify_work/verify.AppImage"
    (cd "$verify_work" && ./verify.AppImage --appimage-extract >/dev/null)
    check_appdir "$verify_work/squashfs-root" "$verify_file"
    echo "Verified $verify_file"
    exit 0
fi

[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die 'Packaging needs Linux x86_64 (Windows: tools\package.ps1).'
for tool in python3 git patchelf objcopy readelf strip curl dpkg dpkg-query ldd sha256sum cmp desktop-file-validate appstreamcli; do
    command -v "$tool" >/dev/null || die "Missing $tool (packaging/linux/ci-deps.sh installs the release environment)"
done
host_glibc=$(getconf GNU_LIBC_VERSION | cut -d' ' -f2)
newer_than "$host_glibc" "$max_glibc" &&
    die "This system has glibc $host_glibc. Release AppImages are built on Ubuntu 20.04 (glibc $max_glibc): see packaging/linux/ci-deps.sh."

release_date=${JPT_RELEASE_DATE:-$(date -u +%Y-%m-%d)}
[[ $release_date =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] || die "JPT_RELEASE_DATE is not YYYY-MM-DD: $release_date"
# The WSL mirror has no .git; leaving it unset uses appimagetool's fixed epoch, as in the other ports.
if [[ -z ${SOURCE_DATE_EPOCH:-} ]] && git_epoch=$(git -C "$root" log -1 --format=%ct 2>/dev/null); then
    export SOURCE_DATE_EPOCH=$git_epoch
fi


step "$asset_prefix $version - Linux AppImage (local only)"
if ((!skip_build)); then
    ((clean)) && rm -rf "$build_dir"
    JPT_FFMPEG_REQUIRE_LGPL=1 JPT_FFMPEG_PINNED=1 bash "$root/tools/test-linux.sh" --config Release --build-dir "$build_dir" --stage
fi
[[ -x $artifact ]] || die "Missing $artifact: run the Linux Release gate first (without --skip-build)"
[[ -f $manifest ]] || die "Missing $manifest: the canonical artifact has no build manifest"

step 'Checking the canonical Release artifact'
python3 -I "$root/tools/linux_artifact.py" verify --elf "$artifact" --manifest "$manifest" || {
    echo "package-linux: the binary is not the validated one; nothing was packaged" >&2
    exit 1
}
python3 -I - "$manifest" "$version" "$program" <<'PY'
import json
import sys

path, version, program = sys.argv[1], sys.argv[2], sys.argv[3]
manifest = json.load(open(path, encoding="utf-8"))
build = manifest.get("build", {})
problems = []
if manifest.get("schema") != 1:
    problems.append("schema is not 1")
if manifest.get("test_result") != "PASS":
    problems.append("the manifest does not record a passing gate")
if build.get("version") != version:
    problems.append("the binary is version %r, CMakeLists.txt says %r" % (build.get("version"), version))
if build.get("config") != "Release":
    problems.append("the binary is a %r build, not Release" % build.get("config"))
if build.get("target") != "linux-x86_64":
    problems.append("the binary target is %r, not linux-x86_64" % build.get("target"))
if build.get("glibc_baseline") != "2.31":
    problems.append("the binary glibc baseline is %r, not 2.31" % build.get("glibc_baseline"))
ffmpeg = manifest.get("ffmpeg") or {}
if not ffmpeg.get("enabled"):
    problems.append("the binary was built without FFmpeg: Smacker video would be missing")
if not ffmpeg.get("lgpl_only"):
    problems.append("the binary FFmpeg is not LGPL-only")
if problems:
    print("package-linux: " + "; ".join(problems), file=sys.stderr)
    sys.exit(1)
print("artifact: %s %s, source fingerprint %s, glibc %s, FFmpeg LGPL-only" % (
    program, build["version"], manifest.get("source_fingerprint", "?")[:12], manifest.get("glibc_max", "?")))
PY
openssl_version=$(sed -n 's/^version=\([0-9][0-9.]*\)$/\1/p' tools/openssl-bootstrap.sh | head -1)
[[ -n $openssl_version ]] || die 'OpenSSL version not found in tools/openssl-bootstrap.sh'
grep -a -q -F "OpenSSL $openssl_version" "$artifact" ||
    die "the Release artifact does not carry OpenSSL $openssl_version (the Bug Report HTTPS needs the pinned static build)"

rm -rf "$appdir" "$symbols"
mkdir -p "$dist" "$symbols" "$appdir/usr/bin" "$appdir/usr/lib" "$appdir/usr/share/licenses/$doc_name" \
         "$appdir/usr/share/doc/$doc_name" "$appdir/usr/share/applications" "$appdir/usr/share/metainfo" \
         "$appdir/usr/share/icons/hicolor/256x256/apps"
rm -f "$dist/$name.AppImage" "$dist/$name.AppImage.sha256" "$dist/$name.AppImage.new"

step "Staging $program without debug information"
objcopy --only-keep-debug "$artifact" "$symbols/$program.debug"
objcopy --strip-debug --add-gnu-debuglink="$symbols/$program.debug" "$artifact" "$appdir/usr/bin/$program"
chmod 755 "$appdir/usr/bin/$program"

step 'Staging the launcher images'
install -d "$appdir/usr/bin/assets/launcher"
for image in header-art.png sponsor-heart.png; do
    install -m644 "assets/launcher/$image" "$appdir/usr/bin/assets/launcher/$image"
done

step 'Staging the libraries it loads'
declare -A seen=()
declare -A owners=()
bundle() {
    local elf=$1 line lib file
    while read -r line; do
        lib=$(awk '/=> \//{print $3}' <<<"$line")
        [[ -n $lib ]] || continue
        file=$(basename -- "$lib")
        [[ -n ${seen[$file]:-} ]] && continue
        seen[$file]=1
        [[ $file =~ $host_libs ]] && continue
        [[ $file =~ $static_only ]] && die "$program links $file dynamically; the Release build must stay static"
        install -m755 "$(readlink -f -- "$lib")" "$appdir/usr/lib/$file"
        bundle "$lib"
        owners[$file]=$lib
    done < <(env -u LD_LIBRARY_PATH ldd "$elf")
}
bundle "$appdir/usr/bin/$program"
for lib in "$appdir"/usr/lib/*.so*; do
    [[ -e $lib ]] || continue
    strip --strip-unneeded "$lib"
    patchelf --remove-rpath "$lib"
    patchelf --force-rpath --set-rpath '$ORIGIN' "$lib"
done
# DT_RPATH (not RUNPATH) is inherited by the libraries' own dependencies and wins over LD_LIBRARY_PATH. The build's
# RUNPATH is removed first: patchelf would otherwise keep its tag.
patchelf --remove-rpath "$appdir/usr/bin/$program"
patchelf --force-rpath --set-rpath '$ORIGIN/../lib' "$appdir/usr/bin/$program"
if compgen -G "$appdir/usr/lib/*.so*" >/dev/null; then
    echo "Bundled: $(cd "$appdir/usr/lib" && ls -- *.so* | tr '\n' ' ')"
else
    echo 'Bundled: (none; SDL3, FFmpeg, OpenSSL, libstdc++ and libgcc are static)'
fi

step 'Staging AppRun, desktop entry, icon, AppStream, licences and README'
install -m755 packaging/linux/AppRun "$appdir/AppRun"
install -m644 "packaging/linux/$desktop_file" "$appdir/$desktop_file"
install -m644 "packaging/linux/$desktop_file" "$appdir/usr/share/applications/$desktop_file"
install -m644 packaging/icons/app-icon-256.png "$appdir/$app_icon"
install -m644 packaging/icons/app-icon-256.png "$appdir/usr/share/icons/hicolor/256x256/apps/$app_icon"
ln -s "$app_icon" "$appdir/.DirIcon"
desktop-file-validate "$appdir/$desktop_file" || die "$desktop_file is not a valid desktop entry"
sed "s/@VERSION@/$version/g; s/@DATE@/$release_date/g" "packaging/linux/$appdata_file" > "$appdir/$metainfo"
chmod 644 "$appdir/$metainfo"
grep -qF "<release version=\"$version\" date=\"$release_date\"/>" "$appdir/$metainfo" || die 'the metainfo has no release entry'
appstreamcli validate --no-net "$appdir/$metainfo" || die "$metainfo does not validate"
sed "s/@VERSION@/$version/g" packaging/README-linux.txt > "$appdir/usr/share/doc/$doc_name/README.txt"

licenses=$appdir/usr/share/licenses/$doc_name
install -m644 packaging/licenses/LICENSE.txt "$licenses/LICENSE.txt"
install -m644 docs/LEGAL.md "$licenses/LEGAL.txt"
install -m644 packaging/licenses/THIRD-PARTY-LICENSES.txt "$licenses/"
install -m644 packaging/licenses/FFmpeg-LGPL-2.1.txt packaging/licenses/FFMPEG-SOURCE.txt \
    packaging/licenses/OpenSSL-LICENSE.txt packaging/licenses/Apache-2.0.txt "$licenses/"
declare -A built_in=(
    [SDL]=external/SDL/LICENSE.txt [Dear-ImGui]=external/imgui/LICENSE.txt
    [pcrecomp]=external/pcrecomp/LICENSE
    [miniz]=launcher/updater/third_party/miniz/LICENSE [nanosvg]=launcher/third_party/nanosvg/LICENSE.txt
    [ASSAO]=third_party/assao/LICENSE [Vulkan-Headers]=third_party/vulkan/LICENSE.txt)
for component in $(printf '%s\n' "${!built_in[@]}" | sort); do
    [[ -f ${built_in[$component]} ]] || die "Missing licence file: ${built_in[$component]}"
    mkdir -p "$licenses/$component"
    install -m644 "${built_in[$component]}" "$licenses/$component/"
done
# A dynamic library the build did not expect is bundled, but only with the licence text of the package that owns it.
if compgen -G "$appdir/usr/lib/*.so*" >/dev/null; then
    manifest=$licenses/SYSTEM-PACKAGES.txt
    {
        echo "Libraries bundled in the AppImage, with the distribution packages they come from and their licence texts."
        echo "Libraries and drivers the player's system provides (glibc, Vulkan, X11/Wayland, audio) are not bundled."
    } > "$manifest"
    for lib in "$appdir"/usr/lib/*.so*; do
        file=$(basename -- "$lib")
        path=${owners[$file]:-}
        [[ -n $path ]] || die "No source recorded for $file"
        package=$(dpkg-query -S "$(readlink -f -- "$path")" "$path" 2>/dev/null | head -1 | cut -d: -f1 || true)
        [[ -n $package ]] || die "No package owns $path: add its licence handling to tools/package-linux.sh"
        version_of=$(dpkg-query -W -f='${Version}' "$package")
        mkdir -p "$licenses/system/$package"
        install -m644 "/usr/share/doc/$package/copyright" "$licenses/system/$package/copyright"
        printf '\n%s (%s %s)\n  Licence text: licenses/system/%s/copyright\n' \
            "$file" "$package" "$version_of" "$package" >> "$manifest"
    done
fi

step 'Checking the AppDir'
check_appdir "$appdir"

step 'Writing the AppImage'
tool=$work/appimagetool-$appimagetool_version-x86_64.AppImage
if [[ ! -x $tool ]]; then
    curl -fL --retry 3 -o "$tool.part" \
        "https://github.com/AppImage/appimagetool/releases/download/$appimagetool_version/appimagetool-x86_64.AppImage"
    echo "$appimagetool_sha256  $tool.part" | sha256sum -c - || die 'appimagetool does not match the pinned checksum'
    chmod +x "$tool.part"
    mv "$tool.part" "$tool"
fi
echo "$appimagetool_sha256  $tool" | sha256sum -c --status - || die "$tool does not match the pinned checksum"
# appimagetool would otherwise download the unpinned "continuous" runtime.
runtime=$work/runtime-jpt-x86_64
bash tools/build-appimage-runtime.sh "$runtime" "$work/appimage-runtime"
# The AppStream file is validated above without the network; appimagetool's own check fetches the project URLs.
ARCH=x86_64 "$tool" --appimage-extract-and-run --no-appstream --runtime-file "$runtime" "$appdir" "$dist/$name.AppImage.new" \
    >"$work/appimagetool.log" 2>&1 || { cat "$work/appimagetool.log" >&2; die 'appimagetool failed'; }
chmod 755 "$dist/$name.AppImage.new"
bash tools/build-appimage-runtime.sh --check "$dist/$name.AppImage.new"
verify_embedded_runtime "$dist/$name.AppImage.new" "$runtime"

step 'Checking the contents of the written AppImage'
rm -rf "$work/written"
mkdir -p "$work/written"
(cd "$work/written" && "$dist/$name.AppImage.new" --appimage-extract >/dev/null)
check_appdir "$work/written/squashfs-root" "$dist/$name.AppImage.new"
diff -r --no-dereference "$appdir" "$work/written/squashfs-root" >/dev/null ||
    die 'the written AppImage does not hold exactly the checked AppDir'
rm -rf "$work/written"
mv -f "$dist/$name.AppImage.new" "$dist/$name.AppImage"
(cd "$dist" && sha256sum "$name.AppImage" > "$name.AppImage.sha256")
(cd "$dist" && sha256sum -c "$name.AppImage.sha256")

printf '\nAPPIMAGE READY (nothing was published)\n'
printf '  AppImage: %s (%s)\n' "$dist/$name.AppImage" "$(du -h "$dist/$name.AppImage" | cut -f1)"
printf '  SHA-256:  %s\n' "$(cut -d' ' -f1 "$dist/$name.AppImage.sha256")"
printf '  Checksum: %s\n' "$dist/$name.AppImage.sha256"
printf '  glibc:    newest version needed %s (baseline %s)\n' "${glibc_newest:-none}" "$max_glibc"
printf '  Symbols:  %s (not in the package)\n' "$symbols/$program.debug"
