#!/usr/bin/env bash
# Release build environment of the Linux AppImage: Ubuntu 20.04, whose glibc (2.31) is the oldest the AppImage runs
# on (tools/package-linux.sh rejects any shipped ELF that needs a newer one). Run once as root on a clean Ubuntu 20.04
# machine, container or WSL distribution (docs/LINUX.md); it needs network access and changes system packages.
# GCC 13 comes from the ubuntu-toolchain-r PPA, whose signing key is checked by fingerprint. CMake 3.31.8 and OpenSSL
# 3.5.9 come from pinned, verified sources: what 20.04's packages ship is too old, and the release links OpenSSL
# statically.
set -euo pipefail

CMAKE_VERSION=3.31.8
CMAKE_SHA256=630615d8e98ac33eba7fbe472626dff5c899c85af3c024585ae109166a6909d0
TOOLCHAIN_KEY=60C317803A41BA51845E371A1E9377A2BA9EF27F

die() { echo "ERROR: $*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

[[ $(id -u) == 0 ]] || die 'run as root'
. /etc/os-release
[[ $ID == ubuntu && $VERSION_ID == 20.04 ]] || die "this is $PRETTY_NAME, not Ubuntu 20.04"
export DEBIAN_FRONTEND=noninteractive LC_ALL=C.UTF-8
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fetch() { # url sha256 file
    curl -fsSL --retry 3 -o "$3" "$1"
    echo "$2  $3" | sha256sum -c --status - || die "$1 does not match its pinned SHA-256"
}
add_key() { # name fingerprint url
    local ring=/usr/share/keyrings/$1.gpg
    curl -fsSL --retry 3 "$3" | gpg --dearmor --yes -o "$ring"
    [[ $(gpg --no-default-keyring --keyring "$ring" --with-colons --list-keys |
         awk -F: '/^pub/ {primary = 1; next} primary && /^fpr/ {print $10; primary = 0}') == "$2" ]] ||
        { rm -f "$ring"; die "the $1 signing key is not (only) $2"; }
}

step 'Base packages'
apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gnupg git make ninja-build pkg-config python3 perl file \
    patchelf binutils rsync xz-utils bzip2 desktop-file-utils appstream

step 'GCC 13 (ubuntu-toolchain-r PPA)'
add_key ubuntu-toolchain-r "$TOOLCHAIN_KEY" "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x$TOOLCHAIN_KEY"
echo 'deb [signed-by=/usr/share/keyrings/ubuntu-toolchain-r.gpg] https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu focal main' \
    > /etc/apt/sources.list.d/ubuntu-toolchain-r.list
apt-get update
apt-get install -y --no-install-recommends gcc-13 g++-13 libstdc++-13-dev
update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 200 --slave /usr/bin/g++ g++ /usr/bin/g++-13

step 'SDL, X11, Wayland and audio development packages'
apt-get install -y --no-install-recommends \
    libvulkan-dev libpng-dev zlib1g-dev \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev libxinerama-dev \
    libxkbcommon-dev libwayland-dev libwayland-bin wayland-protocols libdrm-dev libgbm-dev libegl-dev \
    libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libibus-1.0-dev

step "CMake $CMAKE_VERSION"
fetch "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-linux-x86_64.tar.gz" \
    "$CMAKE_SHA256" "$work/cmake.tar.gz"
rm -rf /opt/cmake
mkdir -p /opt/cmake
tar -xzf "$work/cmake.tar.gz" -C /opt/cmake --strip-components=1
ln -sf /opt/cmake/bin/cmake /opt/cmake/bin/ctest /opt/cmake/bin/cpack /usr/local/bin/

step 'OpenSSL, static (tools/openssl-bootstrap.sh, its version and SHA-256 live there)'
bash "$(dirname -- "$0")/../../tools/openssl-bootstrap.sh" --cc gcc-13 --prefix /opt/jpt-openssl
install -d /usr/local/lib /usr/local/include /usr/local/lib/pkgconfig
ln -sf /opt/jpt-openssl/lib/libssl.a /usr/local/lib/libssl.a
ln -sf /opt/jpt-openssl/lib/libcrypto.a /usr/local/lib/libcrypto.a
ln -sfn /opt/jpt-openssl/include/openssl /usr/local/include/openssl
install -m644 /opt/jpt-openssl/lib/pkgconfig/openssl.pc /opt/jpt-openssl/lib/pkgconfig/libssl.pc \
    /opt/jpt-openssl/lib/pkgconfig/libcrypto.pc /usr/local/lib/pkgconfig/
ldconfig

printf '\nReady: Ubuntu 20.04 (%s), gcc-13, cmake %s, static OpenSSL (pinned by tools/openssl-bootstrap.sh).\n' \
    "$(getconf GNU_LIBC_VERSION)" "$CMAKE_VERSION"
printf 'Build with tools/package-linux.sh.\n'
