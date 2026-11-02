#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Debian package (on the Debian/Ubuntu release it targets: the engine is compiled here)
#   ./packaging/build-deb.sh [out dir]   ->  dvbtx_<VERSION>+<release>_<arch>.deb
# <release>: deb12, deb13, ubuntu24.04... from /etc/os-release (DEB_SUFFIX= to choose; empty: none),
# since the radio plugins link the UHD / SoapySDR library of that release.
# packaging/build-all-debs.sh builds them all in containers.
# Needs: build-essential pkg-config libfftw3-dev, and for the radio plugins libhackrf-dev libuhd-dev
# libsoapysdr-dev (each one optional: a missing one is just not built).
set -eu
cd "$(dirname "$0")/.."
if [ -z "${DEB_SUFFIX+x}" ]; then
    DEB_SUFFIX=$(. /etc/os-release; case "$ID" in debian) echo "+deb${VERSION_ID%%.*}" ;;
                                                  ubuntu) echo "+ubuntu$VERSION_ID" ;; *) echo "+$ID$VERSION_ID" ;; esac)
fi
VERSION=$(cat VERSION)$DEB_SUFFIX
OUT=${1:-.}
ARCH=$(dpkg --print-architecture)
ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT

make -C engine clean >/dev/null
make -C engine CFLAGS="-O3 -g -march=x86-64-v2 -mtune=generic" >/dev/null

lib=$ROOT/usr/lib/dvbtx
install -d "$lib/python/dvbtx/web" "$ROOT/usr/bin" "$ROOT/etc/dvbtx" "$ROOT/etc/sysctl.d" "$ROOT/usr/lib/systemd/system" \
           "$ROOT/usr/share/dvbtx/configs" "$ROOT/usr/share/doc/dvbtx" "$ROOT/DEBIAN"
install -m 755 engine/build/dvbtx-engine "$lib/"
install -m 644 engine/build/dvbtx-sdr-*.so "$lib/"
install -m 644 dvbtx/*.py "$lib/python/dvbtx/"
install -m 644 dvbtx/web/index.html "$lib/python/dvbtx/web/"
install -m 755 packaging/dvbtx "$ROOT/usr/bin/"
install -m 644 packaging/dvbtx.toml "$ROOT/etc/dvbtx/"
install -m 644 packaging/sysctl.conf "$ROOT/etc/sysctl.d/90-dvbtx.conf"
install -m 644 packaging/dvbtx.service packaging/dvbtx@.service packaging/dvbtx-power.service "$ROOT/usr/lib/systemd/system/"
install -m 644 configs/*.toml "$ROOT/usr/share/dvbtx/configs/"
install -m 644 README.md README.fr.md docs/CHANNELS.md "$ROOT/usr/share/doc/dvbtx/"
for m in man/*.[1-8]; do
    sec=${m##*.}
    install -d "$ROOT/usr/share/man/man$sec"
    gzip -9n < "$m" > "$ROOT/usr/share/man/man$sec/${m##*/}.gz"
done
{ printf 'dvbtx (%s) unstable; urgency=medium\n\n  * Built from %s.\n\n' "$VERSION" \
         "$(git describe --always --dirty 2>/dev/null || echo 'the source tree')"
  printf ' -- BILLY Rémi <byhoster.association@gmail.com>  %s\n' "$(date -R)"; } |
    gzip -9n > "$ROOT/usr/share/doc/dvbtx/changelog.Debian.gz"
printf '%s\n' 'Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/' 'Upstream-Name: dvbtx' \
    'Source: https://github.com/TrusterBSW/DVB-Tx' '' 'Files: *' 'Copyright: 2024-2026 BILLY Rémi' 'License: GPL-3+' \
    ' This program is free software: you can redistribute it and/or modify it under the terms of the GNU' \
    ' General Public License as published by the Free Software Foundation, either version 3 of the' \
    ' License, or (at your option) any later version.' ' .' \
    ' On Debian systems, the complete text of the GNU General Public License version 3 can be found in' \
    ' /usr/share/common-licenses/GPL-3.' > "$ROOT/usr/share/doc/dvbtx/copyright"
strip --strip-unneeded "$lib/dvbtx-engine" "$lib"/dvbtx-sdr-*.so

# shared library dependencies: the engine is required, each radio plugin only recommended
DEP="libc6"
REC=""
if command -v dpkg-shlibdeps >/dev/null; then
    tmp=$(mktemp -d)
    mkdir -p "$tmp/debian"
    printf 'Source: dvbtx\n\nPackage: dvbtx\nArchitecture: any\n' > "$tmp/debian/control"
    DEP=$(cd "$tmp" && dpkg-shlibdeps -O "$lib/dvbtx-engine" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')
    for p in "$lib"/dvbtx-sdr-*.so; do
        d=$(cd "$tmp" && dpkg-shlibdeps -O "$p" 2>/dev/null | sed -n 's/^shlibs:Depends=//p' |
            tr ',' '\n' | sed 's/^ *//' | grep -v -E '^libc6|^libgcc|^libstdc' || true)
        [ -n "$d" ] && REC="$REC${REC:+, }$(echo "$d" | paste -sd, - | sed 's/,/, /g')"
    done
    rm -rf "$tmp"
fi
[ -n "$REC" ] || REC="libhackrf0"
REC="$REC, streamlink"      # HLS sources: rides the discontinuities that stall ffmpeg (see the hls option)

install -m 755 packaging/debian/postinst packaging/debian/prerm packaging/debian/postrm "$ROOT/DEBIAN/"
install -m 644 packaging/debian/conffiles "$ROOT/DEBIAN/"
SIZE=$(du -sk --exclude=DEBIAN "$ROOT" | cut -f1)
sed -e "s/@VERSION@/$VERSION/" -e "s/@SIZE@/$SIZE/" -e "s/@ARCH@/$ARCH/" -e "s/@DEPENDS@/$DEP/" \
    -e "s/@RECOMMENDS@/$REC/" packaging/debian/control.in > "$ROOT/DEBIAN/control"

mkdir -p "$OUT"
deb="$OUT/dvbtx_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group -Zxz --build "$ROOT" "$deb" >/dev/null
echo "$deb"
