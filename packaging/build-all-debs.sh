#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the .deb of every supported release in clean containers (docker or podman), then installs
# each one in a fresh container of its release and checks that it works:
#   packaging/build-all-debs.sh [out dir] [image...]     (default dist/, debian:12 debian:13 ubuntu:24.04)
# One package per release: the radio plugins link the UHD and SoapySDR libraries of that release.
set -eu
cd "$(dirname "$0")/.."
OUT=$(realpath -m "${1:-dist}")
[ $# -gt 0 ] && shift
IMAGES=${*:-debian:12 debian:13 ubuntu:24.04}
mkdir -p "$OUT"
DOCKER=$(command -v docker || command -v podman) || { echo "docker or podman needed" >&2; exit 1; }

for img in $IMAGES; do
    echo "== $img: build"
    $DOCKER run --rm -v "$PWD:/src:ro" -v "$OUT:/out" -e OWNER="$(id -u):$(id -g)" "$img" sh -ec '
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends build-essential pkg-config libfftw3-dev libhackrf-dev \
            libuhd-dev libsoapysdr-dev dpkg-dev git ca-certificates >/dev/null
        cp -a /src /build && cd /build
        git config --global --add safe.directory /build
        deb=$(./packaging/build-deb.sh /out)
        chown "$OWNER" "$deb"
        echo "$deb"'

    echo "== $img: install and check"
    $DOCKER run --rm -v "$OUT:/out:ro" "$img" sh -ec '
        export DEBIAN_FRONTEND=noninteractive
        . /etc/os-release
        case "$ID" in debian) s="+deb${VERSION_ID%%.*}" ;; ubuntu) s="+ubuntu$VERSION_ID" ;; esac
        rm -f /etc/dpkg/dpkg.cfg.d/excludes /etc/dpkg/dpkg.cfg.d/docker   # images without man pages
        apt-get update -qq
        apt-get install -y -qq man-db lintian /out/dvbtx_*"$s"_*.deb >/dev/null 2>&1 ||   # with the Recommends:
            apt-get install -y man-db lintian /out/dvbtx_*"$s"_*.deb                     # the radio libraries
        dvbtx --version
        dvbtx -h >/dev/null
        dvbtx check -c /usr/share/dvbtx/configs/hackrf-1mux.toml | tail -1
        dvbtx calc dvbt | head -1
        for f in /usr/lib/dvbtx/dvbtx-engine /usr/lib/dvbtx/dvbtx-sdr-*.so; do
            ldd "$f" | grep "not found" && echo "$f: library missing (a Recommends not installed?)"
        done
        echo "radio plugins: $(cd /usr/lib/dvbtx && ls dvbtx-sdr-*.so | tr "\n" " ")"
        man=$(command -v man.REAL || command -v man)    # minimized Ubuntu images: man is a stub
        for p in dvbtx.1 dvbtx-channel.1 dvbtx.toml.5 dvbtx-sources.7 dvbtx.7 dvbtx-web.7 dvbtx-engine.8 dvbtx-power.8; do
            sec=${p##*.}
            $man -w "$sec" "${p%.*}" 2>/dev/null | grep -q "/man$sec/$p" || echo "missing man page $p"
        done
        for p in dvbtx.1 dvbtx-channel.1 dvbtx.toml.5 dvbtx-sources.7 dvbtx.7 dvbtx-web.7 dvbtx-engine.8 dvbtx-power.8; do
            sec=${p##*.}
            $man -L fr -w "$sec" "${p%.*}" 2>/dev/null | grep -q "/fr/man$sec/$p" || echo "missing French man page $p"
        done
        echo "man pages: $(ls /usr/share/man/man*/dvbtx* | wc -l) in English, $(ls /usr/share/man/fr/man*/dvbtx* 2>/dev/null | wc -l) in French"
        lintian --no-tag-display-limit /out/dvbtx_*"$s"_*.deb 2>&1 | grep -E "^E:" || echo "lintian: no error"'
done
ls -l "$OUT"
