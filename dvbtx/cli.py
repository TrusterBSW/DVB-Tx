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


def config_path(a):
    """file given with -c, else /etc/dvbtx/dvbtx.toml when nothing is set on the command line"""
    path = a.config
    if not path and os.path.exists("/etc/dvbtx/dvbtx.toml") and not getattr(a, "services", None) \
            and getattr(a, "frequency", None) is None and getattr(a, "channel", None) is None:
        path = "/etc/dvbtx/dvbtx.toml"
    return path


def load_cfg(a):
    """merged configuration dict (file, quick options, --set, run options), not yet validated"""
    path = config_path(a)
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
    return cfg


def load_plan(a):
    path = config_path(a)
    plan = cfgmod.build(load_cfg(a))
    # channel changes are written back to the file, unless channels also come from the command line
    if path and not a.services and not any(o.split("=", 1)[0].strip().startswith("carrier.") and ".service" in o
                                            for o in a.set):
        plan.config_path = path
    return plan


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


def cmd_devices(a):
    """radios seen by their own tools (serial numbers to put in args)"""
    import re as _re
    import shutil
    import subprocess
    found = False
    tools = [("hackrf", ["hackrf_info"], r"Serial number: (\w+)", "serial={}"),
             ("uhd", ["uhd_find_devices"], r"serial: (\w+)", "serial={}"),
             ("soapy", ["SoapySDRUtil", "--find"], r"driver = (\w+)", "driver={}")]
    for driver, cmd, rx, fmt in tools:
        if not shutil.which(cmd[0]):
            continue
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        seen = set()
        for m in _re.finditer(rx, out):
            v = m.group(1)
            if driver == "soapy" and (v in ("audio", "rtlsdr", "airspy", "airspyhf", "sdrplay", "remote", "hackrf", "uhd")
                                      or v in seen):
                continue              # receive-only, or better driven by the native plugins
            seen.add(v)
            found = True
            print(f"driver = \"{driver}\"   args = \"{fmt.format(v)}\"")
    if not found:
        print("no radio found (hackrf_info, uhd_find_devices, SoapySDRUtil not installed or nothing plugged)")


# ------------------------------------------------------------------ dvbtx channel

CHANNEL_HELP = """Channels of the running transmitter, changed without touching the others: the change is
applied at once (only that channel is affected) and written to the configuration file. When dvbtx
is not running (no answer on its web address), the configuration file alone is edited: applied at
the next start. CHANNEL is a number (LCN), a service_id (0x0101) or a name.

examples:
  dvbtx channel list
  dvbtx channel add "France 24" https://example.org/live.m3u8 --number 5
  dvbtx channel source 5 "ytdlp:https://www.youtube.com/@SkyNews/live" --option ytdlp_format=b
  dvbtx channel source 5 http://a.example/fhd.ts http://a.example/hd.ts http://b.example/sd.ts
                       (several sources: in order of preference, the next one when the one playing
                       fails, back to a preferred one when it works again; all down: the backup clip)
  dvbtx channel rename 5 "France 24 HD"
  dvbtx channel renumber "France 24 HD" 2 --swap
  dvbtx channel remove 0x0105
"""


def _channel_target(a):
    """(url, token, config path) the command works with"""
    path = config_path(a)
    cfg = None
    if path and os.path.exists(path):
        try:
            cfg = cfgmod.load(path)
        except (cfgmod.ConfigError, OSError):
            cfg = None
    url = a.url
    if not url:
        listen = (cfg or cfgmod.DEFAULTS)["web"].get("listen") or ""
        if listen:
            host, _, port = listen.rpartition(":")
            if host in ("", "0.0.0.0", "*", "::", "[::]"):
                host = "127.0.0.1"
            url = f"http://{host}:{port}"
    token = a.token or os.environ.get("DVBTX_TOKEN") or ((cfg or {}).get("web") or {}).get("token") or ""
    return url, token, path


def _api(url, token, method, path, body=None):
    """(status, answer) from the running transmitter; None if nothing answers at url"""
    import json
    import urllib.error
    import urllib.request
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url.rstrip("/") + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=150) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {"error": e.reason}
    except (urllib.error.URLError, ConnectionError, TimeoutError):
        return None


def _options(items):
    out = {}
    for it in items or []:
        if "=" not in it:
            sys.exit(f"error: --option expects KEY=VALUE, got {it!r}")
        k, v = it.split("=", 1)
        out[k.strip()] = None if v == "" else cfgmod.parse_value(v)
    return out


def _cache(v):
    """--cache: milliseconds, or auto (the key removed: the default)"""
    if v == "auto":
        return v
    try:
        return int(v)
    except ValueError:
        raise argparse.ArgumentTypeError("milliseconds (100-30000) or auto") from None


def _sources(lst):
    """one source, or several in order of preference"""
    return lst[0] if len(lst) == 1 else list(lst)


def _channel_request(a):
    """(method, API path, body) and the equivalent offline operation"""
    from urllib.parse import quote
    from . import channels as chmod
    op = a.channel_cmd
    if op == "add":
        spec = dict(_options(a.option), name=a.name, source=_sources(a.source))
        for k, v in (("lcn", a.number), ("sid", a.sid), ("carrier", a.mux)):
            if v is not None:
                spec[k] = v
        if a.backup is not None:
            spec["backup"] = a.backup
        if a.no_backup:
            spec["backup"] = False
        if a.cache not in (None, "auto"):
            spec["buffer_ms"] = a.cache
        return "POST", "/api/channels", spec, lambda cfg, plan: chmod.add(cfg, plan, spec) + (None,)
    ref = a.channel
    path = "/api/channels/" + quote(str(ref), safe="")
    if op == "remove":
        def off(cfg, plan):
            s = chmod.find(plan, ref)
            return chmod.remove(cfg, plan, ref) + ((s.carrier.index, s.local_index),)
        return "DELETE", path, None, off
    if op == "rename":
        changes = {"name": a.new_name}
    elif op == "renumber":
        changes = {"lcn": a.number, "swap": a.swap}
    elif op == "source":
        changes = dict(_options(a.option), source=_sources(a.source))
        if a.backup is not None:
            changes["backup"] = a.backup
        if a.no_backup:
            changes["backup"] = ""
        if a.cache is not None:
            changes["buffer_ms"] = None if a.cache == "auto" else a.cache
    else:                                   # set
        changes = _options(a.values)
        if not changes:
            sys.exit("error: give KEY=VALUE settings (KEY= removes one)")
    return "PATCH", path, changes, lambda cfg, plan: chmod.update(cfg, plan, ref, changes) + (None,)


def _print_result(res, where):
    for c in res.get("changes") or []:
        print(f"  {c}")
    if not res.get("changes"):
        print("  nothing changed")
    if res.get("warning"):
        print(f"warning: {res['warning']}")
    print(where)


def cmd_channel(a):
    from . import channels as chmod
    if not a.channel_cmd:
        print(CHANNEL_HELP)
        return
    url, token, path = _channel_target(a)
    if a.channel_cmd == "list":
        r = None if a.offline or not url else _api(url, token, "GET", "/api/channels")
        if r and r[0] == 200:
            rows, live = r[1], True
        else:
            if not path:
                sys.exit(f"error: dvbtx does not answer{' on ' + url if url else ''} and no configuration file given (-c)")
            plan = cfgmod.build(cfgmod.load(path))
            rows, live = [chmod.describe(s) for s in plan.services], False
        if a.json:
            import json
            print(json.dumps(rows, indent=1))
            return
        print(f"{'No':>4}  {'SID':6}  {'Name':24} {'Mux':12} {'On':8} Source")
        for c in sorted(rows, key=lambda c: c["lcn"]):
            on = c.get("on") or ("-" if live else "")
            srcs = c.get("sources") or [c["source"]]
            act = c.get("active", 0) if live else 0
            print(f"{c['lcn']:>4}  0x{c['sid']:04X}  {c['name'][:24]:24} {c['carrier_name'][:12]:12} {on:8} "
                  + (f"{srcs[0]}" if len(srcs) == 1 else f"{'*' if act == 0 else ' '}1 {srcs[0]}")
                  + (f"  (backup {c['backup']})" if c.get("backup") else ""))
            for i, x in enumerate(srcs[1:], 1):
                print(f"{'':59} {'*' if act == i else ' '}{i + 1} {x}")
        if not live:
            print(f"(from {path}: dvbtx is not running)")
        return
    method, api_path, body, offline = _channel_request(a)
    r = None if a.offline or not url else _api(url, token, method, api_path, body)
    if r is not None:
        code, res = r
        if code != 200:
            sys.exit(f"error: {res.get('error', code)}")
        _print_result(res, "applied" + (f", saved in the configuration file" if res.get("saved") else ", not saved"))
        return
    if not path or not os.path.exists(path):
        sys.exit(f"error: dvbtx does not answer{' on ' + url if url else ''} and no configuration file (-c) to edit")
    try:
        cfg = cfgmod.load(path)
        plan = cfgmod.build(cfg)
        new_cfg, new_plan, sid, removed = offline(cfg, plan)
        chmod.persist(path, cfg, new_cfg, removed)
    except chmod.ChannelError as e:
        sys.exit(f"error: {e}")
    print(f"dvbtx is not running{' (nothing on ' + url + ')' if url and not a.offline else ''}: {path} updated, "
          "applied at its next start (or now: systemctl reload dvbtx)")


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
        elif std == "dvbt2":
            from . import t2
            p = {k: v for k, v in (("bandwidth", cfgmod.parse_value(a.bandwidth) if a.bandwidth else None),
                                   ("fft", a.fft), ("guard", a.guard), ("pilot", a.pilot),
                                   ("constellation", a.constellation), ("code_rate", a.code_rate),
                                   ("frame", a.frame)) if v is not None}
            if a.extended:
                p["extended"] = a.extended == "yes"
            try:
                info = t2.resolve(**p)
            except ValueError as e:
                sys.exit(f"error: {e}")
            r = float(info["ts_rate"])
            print(f"L_data {info['data_symbols']}, {info['fec_blocks']} FEC blocks, frame {float(info['frame_seconds']) * 1000:.1f} ms, "
                  f"occupied {float(info['occupied_bandwidth']) / 1e6:.3f} MHz")
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
    sub.add_parser("devices", help="list the radios that are plugged in")
    b = sub.add_parser("make-backup", help="create the clip played when a source fails")
    b.add_argument("-o", "--output", default="/var/lib/dvbtx/backup.ts")
    b.add_argument("--text", default="Incident technique - nous revenons dans un instant")
    b.add_argument("--image", help="picture to show instead of the text")
    b.add_argument("--duration", type=int, default=10)
    b.add_argument("--tone", action="store_true", help="1 kHz tone instead of silence")
    ch = sub.add_parser("channel", aliases=["channels"], help="list, add, remove, rename, renumber channels, "
                        "change their source (while running)", description=CHANNEL_HELP,
                        formatter_class=argparse.RawDescriptionHelpFormatter)
    def channel_common(p, dflt):
        kw = {} if dflt else {"default": argparse.SUPPRESS}   # given before or after the command
        p.add_argument("-c", "--config", help="configuration file (default /etc/dvbtx/dvbtx.toml): its web "
                       "address and token are used, and it is edited directly when dvbtx is not running", **kw)
        p.add_argument("--url", help="web address of the running dvbtx (default: web.listen of the configuration)",
                       **kw)
        p.add_argument("--token", help="web.token of the running dvbtx (or DVBTX_TOKEN)", **kw)
        p.add_argument("--offline", action="store_true", help="edit the configuration file only, even if dvbtx "
                       "runs (then apply it with systemctl reload dvbtx)", **kw)
    channel_common(ch, True)
    common = argparse.ArgumentParser(add_help=False)
    channel_common(common, False)
    cs = ch.add_subparsers(dest="channel_cmd", metavar="COMMAND")
    _sub = cs.add_parser
    cs.add_parser = lambda *x, **k: _sub(*x, parents=[common], **k)
    x = cs.add_parser("list", help="the channels, with what each one is playing")
    x.add_argument("--json", action="store_true")
    x = cs.add_parser("add", help="add a channel")
    x.add_argument("name")
    x.add_argument("source", nargs="+", help="any source dvbtx reads: file, URL, ytdlp:..., twitch://..., udp://...; "
                   "several: in order of preference, the next one is used when the one playing fails")
    x.add_argument("--number", "--lcn", type=int, help="channel number (default: the first free one)")
    x.add_argument("--mux", help="multiplex (carrier index or name), needed if there are several")
    x.add_argument("--sid", type=lambda v: int(v, 0), help="service_id (default: the first free one of the multiplex)")
    x.add_argument("--backup", help="clip played when the source fails (default: defaults.backup)")
    x.add_argument("--no-backup", action="store_true")
    x.add_argument("--cache", type=_cache, metavar="MS", help="buffer between the arrival of the source and "
                   "the transmission (buffer_ms): larger, an irregular stream goes through without losses, the "
                   "channel is later than live; auto (default): 6000 for YouTube, Twitch, HLS, 1000 otherwise")
    x.add_argument("--option", action="append", metavar="KEY=VALUE",
                   help="any service setting, e.g. ytdlp_format=b, readrate=false, encode=true (repeatable)")
    x = cs.add_parser("remove", aliases=["rm", "delete"], help="remove a channel")
    x.add_argument("channel")
    x = cs.add_parser("rename", help="give a channel another name")
    x.add_argument("channel")
    x.add_argument("new_name")
    x = cs.add_parser("renumber", help="give a channel another number (LCN)")
    x.add_argument("channel")
    x.add_argument("number", type=int)
    x.add_argument("--swap", action="store_true", help="if the number is taken, give that channel the old number")
    x = cs.add_parser("source", help="play another source on a channel (no cut: the old one plays until the new "
                      "one delivers)")
    x.add_argument("channel")
    x.add_argument("source", nargs="+", help="the new source, or several in order of preference")
    x.add_argument("--backup", help="also change its backup clip")
    x.add_argument("--no-backup", action="store_true", help="no backup clip any more")
    x.add_argument("--cache", type=_cache, metavar="MS", help="buffer between the arrival of the source and "
                   "the transmission (buffer_ms): larger, an irregular stream goes through without losses, the "
                   "channel is later than live; auto (default): 6000 for YouTube, Twitch, HLS, 1000 otherwise")
    x.add_argument("--option", action="append", metavar="KEY=VALUE", help="service settings that go with the "
                   "new source, e.g. ytdlp_format=b (KEY= removes one)")
    x = cs.add_parser("set", help="change any setting of a channel (KEY= removes it)")
    x.add_argument("channel")
    x.add_argument("values", nargs="+", metavar="KEY=VALUE")
    k = sub.add_parser("calc", help="useful MPEG-TS bit rate of a channel")
    k.add_argument("standard", choices=["dvbt", "dvbt2", "dvbc", "dvbs", "dvbs2"])
    k.add_argument("--fft", help="DVB-T2: 1k 2k 4k 8k 16k 32k")
    k.add_argument("--extended", choices=["yes", "no"], help="DVB-T2 extended carriers")
    k.add_argument("--pilot", help="DVB-T2: PP1..PP8")
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
        if getattr(a, "channel_cmd", None) in ("rm", "delete"):
            a.channel_cmd = "remove"
        {"run": cmd_run, "check": cmd_check, "calc": cmd_calc, "epg": cmd_epg, "engine-ini": cmd_engine_ini,
         "make-backup": cmd_make_backup, "devices": cmd_devices, "channel": cmd_channel,
         "channels": cmd_channel}[a.cmd](a)
    except cfgmod.ConfigError as e:
        sys.exit(f"configuration error: {e}")
    except FileNotFoundError as e:
        sys.exit(f"error: {e}")


if __name__ == "__main__":
    main()
