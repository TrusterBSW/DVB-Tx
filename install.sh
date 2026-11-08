#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs dvbtx from its Debian package, on Debian 12 / 13, Proxmox VE 8 / 9 or Ubuntu 24.04 and later:
#
#   curl -fsSL https://raw.githubusercontent.com/TrusterBSW/DVB-Tx/main/install.sh | sudo sh
#   curl -fsSL https://raw.githubusercontent.com/TrusterBSW/DVB-Tx/main/install.sh | sudo sh -s -- --extras
#   sudo ./install.sh --deb dist/dvbtx_0.3.0_amd64.deb        (a package built here, see packaging/)
#
# Options:
#   --deb FILE   install this .deb instead of downloading the latest one
#   --extras     also install streamlink and yt-dlp (HLS without stalls, YouTube and Twitch sources);
#                TSDuck (analysis) is not in the Debian/Ubuntu repositories: https://github.com/tsduck/tsduck/releases
#   --force      go on with a release that is not one of the above
#
# It does not start transmitting: edit /etc/dvbtx/dvbtx.toml first, then
# "sudo systemctl enable --now dvbtx". Closed coax only: radiating on broadcast frequencies is forbidden.
set -eu

BASE=${DVBTX_BASE_URL:-https://github.com/TrusterBSW/DVB-Tx/raw/main/releases}
DEB="" EXTRAS=0 FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
    --deb) DEB=${2:?--deb needs a file}; shift ;;
    --extras) EXTRAS=1 ;;
    --force) FORCE=1 ;;
    -h | --help) sed -n '3,18s/^# \{0,1\}//p' "$0" 2>/dev/null || echo "see the head of install.sh"; exit 0 ;;
    *) echo "install.sh: unknown option $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run it as root (sudo sh, or sudo ./install.sh)"
command -v apt-get >/dev/null || die "apt-get not found: Debian, Ubuntu or Proxmox VE needed"
[ "$(dpkg --print-architecture)" = amd64 ] || die "the package is built for amd64 (x86-64), not $(dpkg --print-architecture)"

# the release
. /etc/os-release
ok=0
case "$ID" in
debian) case "${VERSION_ID:-}" in 12 | 13) ok=1 ;; esac ;;
ubuntu) [ "${VERSION_ID%%.*}" -ge 24 ] 2>/dev/null && ok=1 ;;
esac
what="$PRETTY_NAME"
if [ -e /etc/pve ] || command -v pveversion >/dev/null 2>&1; then
    what="Proxmox VE $(pveversion 2>/dev/null | sed -n 's|^pve-manager/\([0-9.]*\).*|\1|p') on $PRETTY_NAME"
fi
if [ "$ok" = 1 ]; then
    say "system: $what"
elif [ "$FORCE" = 1 ]; then
    say "system: $what (not tested, --force)"
else
    die "$what is not supported (Debian 12/13, Proxmox VE 8/9, Ubuntu 24.04 or later); --force to try anyway"
fi
grep -q -w sse4_2 /proc/cpuinfo || die "this processor lacks SSE4.2 (x86-64-v2): the engine would not run"

# the package
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fetch() {   # URL FILE
    if command -v curl >/dev/null; then curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null; then wget -q "$1" -O "$2"
    else apt-get install -y -qq curl >/dev/null && curl -fsSL "$1" -o "$2"; fi
}
if [ -n "$DEB" ]; then
    [ -f "$DEB" ] || die "$DEB: no such file"
    DEB=$(realpath "$DEB")
    say "package: $DEB"
else
    fetch "$BASE/LATEST" "$TMP/LATEST" || die "cannot download $BASE/LATEST"
    name=$(head -1 "$TMP/LATEST" | tr -d '[:space:]')
    case "$name" in dvbtx_*_amd64.deb) ;; *) die "unexpected LATEST: $name" ;; esac
    say "downloading $name"
    fetch "$BASE/$name" "$TMP/$name" || die "cannot download $BASE/$name"
    fetch "$BASE/SHA256SUMS" "$TMP/SHA256SUMS" || die "cannot download $BASE/SHA256SUMS"
    (cd "$TMP" && grep " $name\$" SHA256SUMS | sha256sum -c --quiet -) || die "$name: checksum mismatch"
    DEB="$TMP/$name"
    chmod 644 "$DEB"
fi

# installed by apt (dependencies, and the radio libraries recommended by the package)
say "installing (apt pulls ffmpeg, python3, the radio libraries)"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y "$DEB"
if [ "$EXTRAS" = 1 ]; then
    for p in streamlink yt-dlp; do
        if apt-cache policy "$p" 2>/dev/null | grep -q 'Candidate: [0-9]'; then
            if apt-get install -y -qq "$p" >/dev/null 2>&1; then say "extra: $p installed"
            else say "extra: $p could not be installed (apt-get install $p to see why)"; fi
        else
            say "extra: $p not in this release's repositories, skipped"
        fi
    done
    command -v tsp >/dev/null ||
        say "TSDuck (analysis, optional) is not in the repositories: https://github.com/tsduck/tsduck/releases"
fi

# what is there
say "$(dvbtx --version)"
plugins=$(cd /usr/lib/dvbtx && for f in dvbtx-sdr-*.so; do
    n=${f#dvbtx-sdr-}; n=${n%.so}; n=${n%%-*}
    ldd "$f" 2>/dev/null | grep -q "not found" || echo "$n"
done | sort -u | tr '\n' ' ')
say "radio drivers usable here: ${plugins:-none}"
if command -v dvbtx >/dev/null && dvbtx devices 2>/dev/null | grep -q '^driver'; then
    say "radios plugged in:"
    dvbtx devices
fi
cat <<'EOF'

Next:
  sudoedit /etc/dvbtx/dvbtx.toml          your radios, multiplexes and channels (man 5 dvbtx.toml)
  dvbtx devices --save NAME               bind the radio NAME to the device plugged in
  dvbtx check                             validate, see the frequency plan
  sudo systemctl enable --now dvbtx       on air, now and at every boot
  http://127.0.0.1:8080/                  the web page (web.listen = "0.0.0.0:8080" for the LAN)
Closed coax only: radiating on broadcast frequencies is forbidden.
EOF
