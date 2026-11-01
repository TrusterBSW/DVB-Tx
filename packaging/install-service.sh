#!/bin/sh
# Installs dvbtx as a systemd service from this source checkout (the .deb ships its own units):
# started at boot, then   systemctl start|stop|restart|status dvbtx   and   journalctl -u dvbtx -f
#
# usage: sudo packaging/install-service.sh [-c CONFIG] [-n NAME] [--no-start]
#   -c CONFIG   configuration file (default /etc/dvbtx/dvbtx.toml)
#   -n NAME     unit name (default dvbtx): several transmitters, each with its own radios and ports
#   --no-start  enabled at boot only, not started now
# Run it again after moving the checkout or changing CONFIG; "systemctl disable --now NAME" and
# removing /etc/systemd/system/NAME.service uninstalls it.
set -e
CONFIG=/etc/dvbtx/dvbtx.toml
NAME=dvbtx
START=1
while [ $# -gt 0 ]; do
    case "$1" in
        -c) CONFIG=$2; shift 2 ;;
        -n) NAME=$2; shift 2 ;;
        --no-start) START=0; shift ;;
        *) sed -n '2,10s/^# \{0,1\}//p' "$0"; exit 2 ;;
    esac
done
[ "$(id -u)" = 0 ] || { echo "run it as root (sudo)" >&2; exit 1; }
SRC=$(cd "$(dirname "$0")/.." && pwd)
CONFIG=$(realpath "$CONFIG")
[ -f "$CONFIG" ] || { echo "no configuration $CONFIG" >&2; exit 1; }
[ -x "$SRC/engine/build/dvbtx-engine" ] || { echo "build the engine first: make -C $SRC/engine" >&2; exit 1; }
PY=$(command -v python3)
"$PY" -c "import sys; sys.path.insert(0, '$SRC'); import dvbtx" || exit 1

# dvbtx writes its work directory, and the configuration when channels change from the web page
# or "dvbtx channel": both writable, the rest of /home and /root read-only
WORK=$(cd "$SRC" && "$PY" -c "
import sys; from dvbtx import config
print(config.load('$CONFIG').get('work_dir') or '')" 2>/dev/null || true)
RW="-$(dirname "$CONFIG")"                # "-": not an error if missing
[ -n "$WORK" ] && RW="$RW -$WORK"

UNIT=/etc/systemd/system/$NAME.service
cat > "$UNIT" <<EOF
# written by $SRC/packaging/install-service.sh
[Unit]
Description=dvbtx DVB transmitter ($CONFIG)
Documentation=file://$SRC/README.md
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=$PY -m dvbtx run --config $CONFIG
# reload = re-read the EPG folder and the channels of the configuration file (applied channel by
# channel); a change of the radios or multiplexes needs a restart
ExecReload=/bin/kill -HUP \$MAINPID
WorkingDirectory=$SRC
# UHD reads ~/.config/uhd.conf (FPGA image of a B210 clone, ...) and needs \$HOME
Environment=HOME=/root PYTHONUNBUFFERED=1 PYTHONPATH=$SRC
KillMode=mixed
TimeoutStopSec=20
Restart=on-failure
RestartSec=5
# real-time priority of the radio and carrier threads (taken by the engine itself)
LimitRTPRIO=99
LimitNICE=-20
LimitMEMLOCK=infinity
NoNewPrivileges=yes
ProtectHome=read-only
ReadWritePaths=$RW
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
if [ $START = 1 ]; then
    systemctl enable --now "$NAME"
else
    systemctl enable "$NAME"
fi
echo "$UNIT installed ($CONFIG, sources $SRC)"
echo "  systemctl start|stop|restart|status $NAME     journalctl -u $NAME -f"
