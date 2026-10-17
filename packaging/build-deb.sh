#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Debian package (on the Debian/Ubuntu release it targets: the engine is compiled here)
#   ./packaging/build-deb.sh [out dir]   ->  dvbtx_<VERSION>_<arch>.deb
# Needs: build-essential pkg-config libfftw3-dev, and for the radio plugins libhackrf-dev libuhd-dev
# libsoapysdr-dev (each one optional: a missing one is just not built).
set -eu
cd "$(dirname "$0")/.."
VERSION=$(cat VERSION)
OUT=${1:-.}
ARCH=$(dpkg --print-architecture)
ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT

make -C engine clean >/dev/null
make -C engine CFLAGS="-O3 -g -march=x86-64-v2 -mtune=generic" >/dev/null

lib=$ROOT/usr/lib/dvbtx
install -d "$lib/python/dvbtx/web" "$ROOT/usr/bin" "$ROOT/etc/dvbtx" "$ROOT/etc/sysctl.d" "$ROOT/lib/systemd/system" \
           "$ROOT/usr/share/dvbtx/configs" "$ROOT/usr/share/doc/dvbtx" "$ROOT/DEBIAN"
install -m 755 engine/build/dvbtx-engine "$lib/"
install -m 644 engine/build/dvbtx-sdr-*.so "$lib/"
install -m 644 dvbtx/*.py "$lib/python/dvbtx/"
install -m 644 dvbtx/web/index.html "$lib/python/dvbtx/web/"
install -m 755 packaging/dvbtx "$ROOT/usr/bin/"
install -m 644 packaging/dvbtx.toml "$ROOT/etc/dvbtx/"
install -m 644 packaging/sysctl.conf "$ROOT/etc/sysctl.d/90-dvbtx.conf"
install -m 644 packaging/dvbtx.service packaging/dvbtx@.service "$ROOT/lib/systemd/system/"
install -m 644 configs/*.toml "$ROOT/usr/share/dvbtx/configs/"
install -m 644 README.md README.fr.md docs/CHANNELS.md "$ROOT/usr/share/doc/dvbtx/"
{ printf 'Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\nUpstream-Name: dvbtx\n\n'
  printf 'Files: *\nCopyright: 2024-2026 BILLY Rémi\nLicense: GPL-3.0-or-later\n'
  sed -e 's/^$/./' -e 's/^/ /' LICENSE; } > "$ROOT/usr/share/doc/dvbtx/copyright"
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

install -m 755 packaging/debian/postinst packaging/debian/prerm packaging/debian/postrm "$ROOT/DEBIAN/"
install -m 644 packaging/debian/conffiles "$ROOT/DEBIAN/"
SIZE=$(du -sk --exclude=DEBIAN "$ROOT" | cut -f1)
sed -e "s/@VERSION@/$VERSION/" -e "s/@SIZE@/$SIZE/" -e "s/@ARCH@/$ARCH/" -e "s/@DEPENDS@/$DEP/" \
    -e "s/@RECOMMENDS@/$REC/" packaging/debian/control.in > "$ROOT/DEBIAN/control"

mkdir -p "$OUT"
deb="$OUT/dvbtx_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group -Zxz --build "$ROOT" "$deb" >/dev/null
echo "$deb"
