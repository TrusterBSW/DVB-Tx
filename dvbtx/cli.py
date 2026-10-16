"""Command line: dvbtx [run|check|calc|epg|engine-ini] ..."""

import argparse
import datetime as dt
import logging
import os
import sys

from . import __version__
from . import capacity
from . import config as cfgmod

QUICK = ["driver", "args", "gain", "amp", "rate", "center", "standard", "frequency", "channel", "bandwidth",
         "mode", "constellation", "code_rate", "guard", "tsid"]


def _add_config_args(p):
    p.add_argument("-c", "--config", help="TOML configuration file (default /etc/dvbtx/dvbtx.toml if present)")
    p.add_argument("-s", "--set", action="append", default=[], metavar="KEY=VALUE",
                   help="override any setting, e.g. radio.0.gain=40 or carrier.0.service.1.source=file.ts")
    g = p.add_argument_group("quick setup (one radio, one carrier) without or on top of a file")
    g.add_argument("--driver", help="radio driver: hackrf, uhd, soapy, null")
    g.add_argument("--args", help="radio arguments, e.g. serial=...")
    g.add_argument("--gain", type=float, help="TX gain (dB)")
    g.add_argument("--amp", action="store_true", default=None, help="HackRF RF amplifier on")
    g.add_argument("--rate", type=cfgmod.parse_value, help="radio sample rate (default: automatic)")
    g.add_argument("--center", type=cfgmod.parse_value, help="radio LO frequency (default: automatic)")
    g.add_argument("--standard", help="dvbt (default) or ts")
    g.add_argument("--frequency", type=cfgmod.parse_value, help="carrier frequency, e.g. 482MHz")
    g.add_argument("--channel", type=int, help="UHF/VHF channel number instead of --frequency")
    g.add_argument("--bandwidth", type=cfgmod.parse_value, help="channel bandwidth, e.g. 8MHz")
    g.add_argument("--mode", help="2k or 8k")
    g.add_argument("--constellation", help="qpsk, 16qam, 64qam")
    g.add_argument("--code-rate", dest="code_rate", help="1/2 2/3 3/4 5/6 7/8")
    g.add_argument("--guard", help="1/4 1/8 1/16 1/32")
    g.add_argument("--tsid", type=int, help="transport_stream_id")
    g.add_argument("--service", action="append", dest="services", metavar="NAME=SOURCE[,key=value]",
                   help="add a service, e.g. --service 'Mire=lavfi:testsrc2,lcn=5'")
    o = p.add_argument_group("run options")
    o.add_argument("--work-dir", help="runtime directory (engine.ini, SI tables)")
    o.add_argument("--web", metavar="HOST:PORT", help="monitoring page address (none to disable)")
    o.add_argument("--epg-dir", help="folder of XMLTV / TSDuck EIT files")
    o.add_argument("--analysis-export", action="append", metavar="URL",
                   help="send the TSDuck analysis (JSON) to udp://host:port or tcp://host:port (repeatable)")
    o.add_argument("--stats-export", action="append", metavar="URL",
                   help="send the engine statistics + alerts (JSON, every second) to udp:// or tcp://")
    o.add_argument("--no-tsduck", action="store_true", help="do not run the TSDuck analysers")
    o.add_argument("-v", "--verbose", action="store_true")


def load_plan(a):
    path = a.config
    if not path and os.path.exists("/etc/dvbtx/dvbtx.toml") and not a.services and a.frequency is None and a.channel is None:
        path = "/etc/dvbtx/dvbtx.toml"
    quick = {k: getattr(a, k, None) for k in QUICK}
    quick["services"] = a.services
    if not any(v is not None for k, v in quick.items() if k != "services") and not a.services:
        quick = None
    cfg = cfgmod.load(path, a.set, quick)
    if a.work_dir:
        cfg["work_dir"] = a.work_dir
    if a.web:
        cfg["web"]["listen"] = "" if a.web == "none" else a.web
        cfg["web"]["enabled"] = a.web != "none"
    if a.epg_dir:
        cfg["epg"]["dir"] = a.epg_dir
    if a.analysis_export:
        cfg["monitoring"]["export"] = list(cfg["monitoring"]["export"]) + a.analysis_export
        if cfg["monitoring"]["tsduck"] is False:
            cfg["monitoring"]["tsduck"] = True
    if a.stats_export:
        cfg["monitoring"]["stats_export"] = list(cfg["monitoring"]["stats_export"]) + a.stats_export
    if a.no_tsduck:
        cfg["monitoring"]["tsduck"] = False
    return cfgmod.build(cfg)


def cmd_run(a):
    from .supervisor import Supervisor
    plan = load_plan(a)
    os.makedirs(plan.work_dir, exist_ok=True)
    logging.getLogger("dvbtx").info("dvbtx %s\n%s", __version__, cfgmod.describe(plan))
    Supervisor(plan, a).run()


def cmd_check(a):
    plan = load_plan(a)
    print(cfgmod.describe(plan))
    for c in plan.carriers:
        n = len(c.services)
        if n:
            print(f"   {c.name}: about {c.ts_rate * (1 - c.si_reserve) / n / 1e6:.2f} Mbit/s per service if shared equally")
    print("configuration OK")


def cmd_engine_ini(a):
    plan = load_plan(a)
    out = a.output or os.path.join(plan.work_dir, "engine.ini")
    cfgmod.write_engine_ini(plan, out, os.path.join(plan.work_dir, "si"))
    print(out)


def cmd_epg(a):
    from . import epg as epgmod
    plan = load_plan(a)
    e = plan.cfg["epg"]
    store = epgmod.EpgStore(e.get("dir"), e.get("urls"), plan.services, plan.tz, int(e["days"]))
    store.refresh(force=True)
    now = dt.datetime.now(dt.timezone.utc)
    for s in plan.services:
        evs = store.events(s.sid)
        pres, fol, _ = epgmod.present_following(evs, now)
        print(f"LCN {s.lcn:3d} {s.name:<24} {len(evs):4d} events  now: {pres['title'] if pres else '-'}  "
              f"next: {fol['title'] if fol else '-'}")
    for err in store.errors:
        print("error:", err)


def cmd_make_backup(a):
    """10 s looping clip shown when a source fails: a picture or a text on a dark background"""
    import shutil
    import subprocess
    ff = shutil.which("ffmpeg")
    if not ff:
        sys.exit("error: ffmpeg not found")
    if a.image:
        vin = ["-loop", "1", "-framerate", "25", "-i", a.image]
        vf = "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,setsar=1,format=yuv420p"
    else:
        vin = ["-f", "lavfi", "-i", "color=c=0x1d2330:s=1920x1080:r=25"]
        text = a.text.replace(":", "\\:").replace("'", "\u2019")
        size = int(min(90, 3300 / max(1, len(a.text))))
        vf = f"drawtext=text='{text}':fontsize={size}:fontcolor=white:x=(w-text_w)/2:y=(h-text_h)/2,format=yuv420p"
    ain = ["-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000" if a.tone else "anullsrc=r=48000:cl=stereo"]
    base = [ff, "-hide_banner", "-loglevel", "error", "-y"] + vin + ain + ["-t", str(a.duration)]
    enc = ["-c:v", "libx264", "-preset", "medium", "-tune", "stillimage", "-crf", "30", "-g", "25", "-keyint_min", "25",
           "-c:a", "aac", "-b:a", "96k", "-ac", "2", "-f", "mpegts", a.output]
    r = subprocess.run(base + ["-vf", vf] + enc)
    if r.returncode and not a.image:      # ffmpeg without drawtext: plain background
        r = subprocess.run(base + ["-vf", "format=yuv420p"] + enc)
    if r.returncode:
        sys.exit("error: ffmpeg failed")
    print(a.output)


def cmd_calc(a):
    std = a.standard
    try:
        if std == "dvbt":
            bw = int(cfgmod.parse_value(a.bandwidth or "8MHz"))
            if a.table:
                print(f"DVB-T {bw / 1e6:g} MHz {a.mode or '8k'}, useful rate (Mbit/s, 188-byte packets)")
                print(f"{'':8} {'':5} " + "".join(f"{g:>8}" for g in capacity.DVBT_GUARDS))
                for c, cr, rates in capacity.dvbt_table(bw, a.mode or "8k"):
                    print(f"{c:8} {cr:5} " + "".join(f"{r / 1e6:8.2f}" for r in rates))
                return
            r = capacity.dvbt_rate(bw, a.mode or "8k", a.constellation or "64qam", a.code_rate or "7/8", a.guard or "1/32")
        elif std == "dvbc":
            r = capacity.dvbc_rate(cfgmod.parse_value(a.symbol_rate or "6.9M"), a.constellation or "256qam")
        elif std == "dvbs":
            r = capacity.dvbs_rate(cfgmod.parse_value(a.symbol_rate or "27.5M"), a.code_rate or "3/4")
        elif std == "dvbs2":
            r = capacity.dvbs2_rate(cfgmod.parse_value(a.symbol_rate or "27.5M"), a.modulation or "8psk",
                                    a.code_rate or "3/4", not a.no_pilots, a.frame or "normal")
        else:
            r = capacity.rate(std)
    except capacity.CapacityError as e:
        sys.exit(f"error: {e}")
    print(f"{r:.0f} bit/s  ({r / 1e6:.3f} Mbit/s useful TS rate, PSI/SI included)")
    if a.services:
        print(f"{a.services} services: {r * 0.97 / a.services / 1e6:.3f} Mbit/s each (3 % kept for PSI/SI)")


def main(argv=None):
    p = argparse.ArgumentParser(prog="dvbtx", description="DVB transmitter: live/file/device sources -> SDR")
    p.add_argument("--version", action="version", version=f"dvbtx {__version__}")
    sub = p.add_subparsers(dest="cmd")
    r = sub.add_parser("run", help="run the transmitter (default)")
    _add_config_args(r)
    c = sub.add_parser("check", help="validate the configuration and show the frequency plan")
    _add_config_args(c)
    e = sub.add_parser("engine-ini", help="write the engine configuration")
    _add_config_args(e)
    e.add_argument("-o", "--output")
    g = sub.add_parser("epg", help="show what the EPG folder gives for each service")
    _add_config_args(g)
    b = sub.add_parser("make-backup", help="create the clip played when a source fails")
    b.add_argument("-o", "--output", default="/var/lib/dvbtx/backup.ts")
    b.add_argument("--text", default="Incident technique - nous revenons dans un instant")
    b.add_argument("--image", help="picture to show instead of the text")
    b.add_argument("--duration", type=int, default=10)
    b.add_argument("--tone", action="store_true", help="1 kHz tone instead of silence")
    k = sub.add_parser("calc", help="useful MPEG-TS bit rate of a channel")
    k.add_argument("standard", choices=["dvbt", "dvbc", "dvbs", "dvbs2"])
    k.add_argument("--bandwidth")
    k.add_argument("--mode", choices=["2k", "8k"])
    k.add_argument("--constellation")
    k.add_argument("--modulation")
    k.add_argument("--code-rate", dest="code_rate")
    k.add_argument("--guard")
    k.add_argument("--symbol-rate", dest="symbol_rate")
    k.add_argument("--frame", choices=["normal", "short"])
    k.add_argument("--no-pilots", action="store_true")
    k.add_argument("--services", type=int, help="also show the share per service")
    k.add_argument("--table", action="store_true", help="DVB-T: every constellation / code rate / guard")

    argv = sys.argv[1:] if argv is None else argv
    if not argv or argv[0].startswith("-") and argv[0] not in ("-h", "--help", "--version"):
        argv = ["run"] + argv
    a = p.parse_args(argv)
    if a.cmd is None:
        a = p.parse_args(["run"])
    logging.basicConfig(level=logging.DEBUG if getattr(a, "verbose", False) else logging.INFO,
                        format="%(asctime)s %(levelname).1s %(name)s: %(message)s", datefmt="%H:%M:%S")
    try:
        {"run": cmd_run, "check": cmd_check, "calc": cmd_calc, "epg": cmd_epg, "engine-ini": cmd_engine_ini,
         "make-backup": cmd_make_backup}[a.cmd](a)
    except cfgmod.ConfigError as e:
        sys.exit(f"configuration error: {e}")
    except FileNotFoundError as e:
        sys.exit(f"error: {e}")


if __name__ == "__main__":
    main()
