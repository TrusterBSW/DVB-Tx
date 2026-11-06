#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds ONE .deb for Debian 12 and 13, Proxmox VE 8 and 9 (same userland) and Ubuntu 24.04, in clean
# containers (docker or podman), then installs it in a fresh container of each release and checks it:
#   packaging/build-all-debs.sh [out dir]          (default dist/)
# The engine and the HackRF / SoapySDR / null plugins are built on the oldest release (Debian 12: its
# glibc and library sonames hold on the newer ones). Only UHD changes soname from release to release
# (libuhd 4.3, 4.6, 4.8): its plugin is built once per release, shipped as dvbtx-sdr-uhd-<version>.so,
# and the engine loads the one whose library is installed.
# BUILD_IMAGE (debian:12), UHD_IMAGES ("debian:12 ubuntu:24.04 debian:13"), TEST_IMAGES (the same) to change.
set -eu
cd "$(dirname "$0")/.."
OUT=$(realpath -m "${1:-dist}")
BUILD_IMAGE=${BUILD_IMAGE:-debian:12}
UHD_IMAGES=${UHD_IMAGES:-debian:12 ubuntu:24.04 debian:13}
TEST_IMAGES=${TEST_IMAGES:-$UHD_IMAGES}
DOCKER=$(command -v docker || command -v podman) || { echo "docker or podman needed" >&2; exit 1; }
VARIANTS="$OUT/uhd-plugins"
rm -rf "$VARIANTS"
mkdir -p "$OUT" "$VARIANTS"

# 1. the UHD plugin against each release's libuhd, and the package that brings that library
for img in $UHD_IMAGES; do
    echo "== $img: UHD plugin"
    $DOCKER run --rm -v "$PWD:/src:ro" -v "$VARIANTS:/out" -e OWNER="$(id -u):$(id -g)" "$img" sh -ec '
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends build-essential pkg-config libuhd-dev dpkg-dev >/dev/null
        cp -a /src/engine /src/VERSION /tmp/ && cd /tmp/engine
        rm -rf build && make build/dvbtx-sdr-uhd.so CFLAGS="-O3 -g -march=x86-64-v2 -mtune=generic" >/dev/null
        v=$(pkg-config --modversion uhd | cut -d. -f1-2)
        f=/out/dvbtx-sdr-uhd-$v.so
        install -m 644 build/dvbtx-sdr-uhd.so "$f"
        strip --strip-unneeded "$f"
        mkdir -p /tmp/d/debian && printf "Source: x\n\nPackage: x\nArchitecture: any\n" > /tmp/d/debian/control
        (cd /tmp/d && dpkg-shlibdeps -O "$f") | sed -n "s/^shlibs:Depends=//p" | tr "," "\n" | sed "s/^ *//" |
            grep "^libuhd" >> /out/recommends
        chown "$OWNER" "$f" /out/recommends
        echo "$f: $(tail -1 /out/recommends)"'
done

# 2. the package, on the oldest release, with every UHD plugin
echo "== $BUILD_IMAGE: package"
$DOCKER run --rm -v "$PWD:/src:ro" -v "$OUT:/out" -e OWNER="$(id -u):$(id -g)" "$BUILD_IMAGE" sh -ec '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends build-essential pkg-config libfftw3-dev libhackrf-dev \
        libuhd-dev libsoapysdr-dev dpkg-dev git ca-certificates >/dev/null
    cp -a /src /build && cd /build
    git config --global --add safe.directory /build
    deb=$(DEB_SUFFIX= UHD_VARIANTS=/out/uhd-plugins ./packaging/build-deb.sh /out)
    chown "$OWNER" "$deb"
    echo "$deb"
    dpkg-deb --field "$deb" Depends Recommends'
DEB=$(ls -t "$OUT"/dvbtx_"$(cat VERSION)"_*.deb | head -1)

# 3. installed in a fresh container of each release
for img in $TEST_IMAGES; do
    echo "== $img: install and check $(basename "$DEB")"
    $DOCKER run --rm -v "$OUT:/out:ro" -v "$PWD/install.sh:/install.sh:ro" -e DEB="/out/$(basename "$DEB")" "$img" sh -ec '
        export DEBIAN_FRONTEND=noninteractive
        rm -f /etc/dpkg/dpkg.cfg.d/excludes /etc/dpkg/dpkg.cfg.d/docker   # images without man pages
        apt-get update -qq
        apt-get install -y -qq man-db lintian >/dev/null
        sh /install.sh --deb "$DEB" --extras > /tmp/install.log 2>&1 || { tail -30 /tmp/install.log; exit 1; }
        grep "==>" /tmp/install.log | sed "s/\x1b\[[0-9;]*m//g"
        dvbtx --version
        dvbtx -h >/dev/null
        dvbtx check -c /usr/share/dvbtx/configs/hackrf-1mux.toml | tail -1
        dvbtx calc dvbt | head -1
        ldd /usr/lib/dvbtx/dvbtx-engine | grep "not found" && echo "dvbtx-engine: library missing"
        # each radio plugin loads (no device here: the error must come from the device, not from loading)
        printf "[engine]\n[radio 0]\ndriver = %s\nfreq = 1200000000\nrate = 10000000\n" > /tmp/t.ini
        for drv in null hackrf soapy uhd; do
            sed -i "s/^driver = .*/driver = $drv/" /tmp/t.ini
            msg=$(/usr/lib/dvbtx/dvbtx-engine --probe /tmp/t.ini 0 2>&1 | grep -v "^\[INFO\]") || true
            if printf "%s\n" "$msg" | grep -q "^radio driver"; then
                echo "plugin $drv: NOT LOADED: $msg"; exit 1
            fi
            echo "plugin $drv: loads ($(printf "%s\n" "$msg" | grep -m1 . || echo "device answers"))"
        done
        man=$(command -v man.REAL || command -v man)    # minimized Ubuntu images: man is a stub
        for p in dvbtx.1 dvbtx-channel.1 dvbtx.toml.5 dvbtx-sources.7 dvbtx.7 dvbtx-web.7 dvbtx-engine.8 dvbtx-power.8; do
            sec=${p##*.}
            $man -w "$sec" "${p%.*}" 2>/dev/null | grep -q "/man$sec/$p" || echo "missing man page $p"
            $man -L fr -w "$sec" "${p%.*}" 2>/dev/null | grep -q "/fr/man$sec/$p" || echo "missing French man page $p"
        done
        echo "man pages: $(ls /usr/share/man/man*/dvbtx* | wc -l) in English, $(ls /usr/share/man/fr/man*/dvbtx* 2>/dev/null | wc -l) in French"
        lintian --no-tag-display-limit "$DEB" 2>&1 | grep -E "^E:" || echo "lintian: no error"'
done
rm -rf "$VARIANTS"
ls -l "$DEB"
