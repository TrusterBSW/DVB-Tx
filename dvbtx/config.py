"""Configuration: TOML file and/or command line -> validated plan of radios, carriers, services,
inputs; and the INI file of the C engine.

Any number of radios, carriers per radio and services per carrier: nothing is fixed in advance.
"""

import copy
import math
import os
import re
import tomllib
from dataclasses import dataclass, field
from zoneinfo import ZoneInfo

from . import capacity

DEFAULTS = {
    "network": {
        "name": "dvbtx", "network_id": 0x20FA, "onid": 0x20FA, "provider": "dvbtx", "country": "FRA",
        "timezone": "Europe/Paris", "charset": "iso8859-9", "lcn_pds": 0x28, "frequency_in_nit": True,
        "sdt_other": False, "nit_period_ms": 2000, "sdt_period_ms": 1000,
    },
    "epg": {"dir": "", "urls": [], "url_refresh_s": 3600, "days": 7, "pf_cycle_ms": 1000,
            "schedule_cycle_ms": 10000, "schedule": True},
    "web": {"listen": "127.0.0.1:8080", "enabled": True},
    "monitoring": {"tsduck": "auto", "interval": 5, "export": [], "stats_export": [], "tsp": "tsp"},
    "alerts": {"margin_min_ms": 150, "overload_percent": 98.0, "input_down_s": 10, "late_ms": 300, "hold_s": 4,
               "webhook": "", "command": "", "repeat_s": 300},
    "ingest": {"base_port": 20100, "monitor_base_port": 20900, "ffmpeg": "ffmpeg", "streamlink": "streamlink",
               "ytdlp": "yt-dlp", "restart_min_s": 1, "restart_max_s": 30, "loglevel": "warning"},
    "engine": {"binary": "", "log_level": "info", "stats_interval_ms": 1000},
    "defaults": {
        "backup": "", "buffer_ms": 1000, "switch_timeout_ms": 3000, "return_ms": 10000, "max_late_ms": 600,
        "early_ms": 100, "pcr_max_ms": 35, "priority": 5, "pmt_period_ms": 100, "mux_delay": 0.7, "ring_pkts": 65536,
        "readrate": "auto", "map": ["0:v:0?", "0:a?"], "loop": True,
        "copy_audio_codecs": ["aac", "mp2", "mp3", "ac3", "eac3", "aac_latm"],
        "encode": {"video_codec": "libx264", "preset": "veryfast", "video_bitrate": "auto", "gop_s": 1.0,
                   "audio_codec": "aac", "audio_bitrate": "128k", "audio_rate": 48000, "pix_fmt": "yuv420p",
                   "size": "", "fps": "", "hwaccel": "", "tune": "", "deinterlace": False,
                   "max_auto_bitrate": "8M"},
    },
    "work_dir": "/run/dvbtx",
}

RADIO_DEFAULTS = {"driver": "hackrf", "args": "", "gain": 30, "amp": False, "ppm": 0.0, "center": 0, "rate": 0,
                  "bandwidth": 0, "backoff_db": 13.0, "buffer_ms": 50, "clock": "internal", "lo_offset": "auto"}
CARRIER_DEFAULTS = {"standard": "dvbt", "bandwidth": 8_000_000, "mode": "8k", "constellation": "64qam",
                    "code_rate": "7/8", "guard": "1/32", "cell_id": 0, "gain_db": 0.0, "monitor": [],
                    "si_reserve": 0.03}

HACKRF_FILTERS = [1.75e6, 2.5e6, 3.5e6, 5e6, 5.5e6, 6e6, 7e6, 8e6, 9e6, 10e6, 12e6, 14e6, 15e6, 20e6, 24e6, 28e6]
DRIVER_RATES = {"hackrf": (8e6, 20e6), "uhd": (0.2e6, 61.44e6), "soapy": (0.1e6, 1e9), "null": (1, 1e10)}


def occupied_half(car):
    """half of the occupied bandwidth of a carrier"""
    if car.standard == "dvbt2":
        from . import t2
        return t2.occupied_bandwidth(**car.params) / 2
    if car.standard == "dvbt":
        bw = int(car.params.get("bandwidth", 8_000_000))
        fft = capacity.DVBT_MODES[car.params.get("mode", "8k")]
        return (6817 if fft == 8192 else 1705) * capacity.dvbt_spacing(bw, car.params.get("mode", "8k")) / 2
    return float(car.params["symbol_rate"]) * (1 + float(car.params.get("rolloff", 0.15))) / 2


class ConfigError(ValueError):
    pass


def _merge(dst, src):
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            _merge(dst[k], v)
        else:
            dst[k] = copy.deepcopy(v)
    return dst


def parse_value(text):
    """TOML-ish value from the command line: numbers, booleans, arrays; else a string."""
    t = text.strip()
    m = re.fullmatch(r"([0-9.]+)\s*([kKmMgG])(?:Hz|hz|b|bps)?", t)
    if m:
        return float(m.group(1)) * {"k": 1e3, "m": 1e6, "g": 1e9}[m.group(2).lower()]
    try:
        return tomllib.loads(f"v = {t}")["v"]
    except tomllib.TOMLDecodeError:
        return t


def set_path(cfg, path, value):
    """--set carrier.0.service.2.source=... (lists indexed by number, created on demand)."""
    keys = path.split(".")
    cur = cfg
    for i, k in enumerate(keys):
        last = i == len(keys) - 1
        if isinstance(cur, list):
            idx = int(k)
            while len(cur) <= idx:
                cur.append({})
            if last:
                cur[idx] = value
            else:
                cur = cur[idx]
            continue
        if last:
            cur[k] = value
        else:
            nxt = keys[i + 1]
            if k not in cur:
                cur[k] = [] if nxt.isdigit() else {}
            cur = cur[k]


def load(path=None, overrides=(), quick=None):
    """Merged configuration dict (defaults < file < quick options < --set)."""
    cfg = copy.deepcopy(DEFAULTS)
    cfg["radio"], cfg["carrier"] = [], []
    if path:
        with open(path, "rb") as f:
            try:
                user = tomllib.load(f)
            except tomllib.TOMLDecodeError as e:
                raise ConfigError(f"{path}: {e}") from e
        _merge(cfg, user)
    if quick:
        _apply_quick(cfg, quick)
    for ov in overrides:
        if "=" not in ov:
            raise ConfigError(f"--set expects key=value, got {ov!r}")
        k, v = ov.split("=", 1)
        set_path(cfg, k.strip(), parse_value(v))
    return cfg


def _apply_quick(cfg, q):
    """Command line only setup: one radio, one carrier, services given with --service."""
    if not cfg["radio"]:
        cfg["radio"].append({"name": "radio0"})
    if not cfg["carrier"]:
        cfg["carrier"].append({"name": "Mux 1", "radio": 0})
    r, c = cfg["radio"][0], cfg["carrier"][0]
    for k in ("driver", "args", "gain", "amp", "rate", "center"):
        if q.get(k) is not None:
            r[k] = q[k]
    for k in ("standard", "frequency", "channel", "bandwidth", "mode", "constellation", "code_rate", "guard", "tsid"):
        if q.get(k) is not None:
            c[k] = q[k]
    for i, s in enumerate(q.get("services") or []):
        c.setdefault("service", []).append(parse_service_arg(s))


def parse_service_arg(s):
    """'Name=source' or 'Name=source,lcn=3,backup=file.ts' (commas inside the source: use ;; as separator)."""
    if "=" not in s:
        raise ConfigError(f"--service expects NAME=SOURCE[,key=value...], got {s!r}")
    name, rest = s.split("=", 1)
    sep = ";;" if ";;" in rest else ","
    parts = rest.split(sep)
    src = parts[0]
    svc = {"name": name.strip(), "source": src.strip()}
    for p in parts[1:]:
        if "=" in p:
            k, v = p.split("=", 1)
            svc[k.strip()] = parse_value(v)
        else:                               # a comma that belonged to the source
            svc["source"] += "," + p
    return svc


# ------------------------------------------------------------------ plan objects

@dataclass
class Input:
    index: int
    name: str
    addr: str
    source: str
    kind: str              # ffmpeg | direct
    service: "Service" = None
    role: str = "primary"  # primary | backup
    options: dict = field(default_factory=dict)


@dataclass
class Service:
    index: int             # global index
    carrier: "Carrier"
    local_index: int
    name: str
    provider: str
    sid: int
    lcn: int
    visible: bool
    type: object           # int or "auto"
    source: str
    backup: str
    epg_id: str
    pid_base: int
    opts: dict
    input: Input = None
    backup_input: Input = None
    detected_type: int = None


@dataclass
class Carrier:
    index: int
    name: str
    radio: "Radio"
    standard: str
    frequency: float
    params: dict
    tsid: int
    onid: int
    ts_rate: float
    offset: float = 0.0
    services: list = field(default_factory=list)
    monitor: list = field(default_factory=list)
    output: str = ""


@dataclass
class Radio:
    index: int
    name: str
    driver: str
    args: str
    gain: float
    amp: bool
    ppm: float
    center: float
    rate: float
    bandwidth: float
    backoff_db: float
    buffer_ms: int
    clock: str
    carriers: list = field(default_factory=list)
    nfft: int = 0


@dataclass
class Plan:
    cfg: dict
    radios: list
    carriers: list
    services: list
    inputs: list
    tz: object

    @property
    def work_dir(self):
        return self.cfg["work_dir"]


def channel_frequency(ch, band="eu"):
    """European UHF (21-69) / VHF band III (5-12) channel centre frequency."""
    ch = int(ch)
    if 21 <= ch <= 69:
        return (306 + 8 * ch) * 1e6
    if 5 <= ch <= 12:
        return (142.5 + 7 * ch) * 1e6
    raise ConfigError(f"unknown channel {ch} (UHF 21-69, VHF 5-12)")


def _num(v, what):
    if isinstance(v, (int, float)):
        return v
    if isinstance(v, str):
        p = parse_value(v)
        if isinstance(p, (int, float)):
            return p
    raise ConfigError(f"{what}: expected a number, got {v!r}")


def _smooth_size(n, multiple):
    """smallest m >= n, multiple of `multiple`, whose other factors are 2, 3, 5, 7 (fast FFT)."""
    m = max(multiple, int(math.ceil(n / multiple)) * multiple)
    while True:
        k = m
        for p in (2, 3, 5, 7):
            while k % p == 0:
                k //= p
        if k == 1:
            return m
        m += multiple


def build(cfg):
    """Validates the configuration and computes everything the engine and ingest need."""
    net = cfg["network"]
    try:
        tz = ZoneInfo(net["timezone"])
    except Exception as e:
        raise ConfigError(f"network.timezone: {e}") from e
    if not cfg.get("carrier"):
        raise ConfigError("no carrier defined ([[carrier]] in the file or --frequency/--service)")

    radios = []
    for i, r in enumerate(cfg.get("radio", [])):
        rr = dict(RADIO_DEFAULTS, **r)
        radios.append(Radio(i, str(rr.get("name", f"radio{i}")), rr["driver"], str(rr["args"]),
                            float(_num(rr["gain"], "gain")), bool(rr["amp"]), float(rr["ppm"]),
                            float(_num(rr["center"], "center")), float(_num(rr["rate"], "rate")),
                            float(_num(rr["bandwidth"], "bandwidth")), float(rr["backoff_db"]),
                            int(rr["buffer_ms"]), str(rr["clock"])))
        radios[-1].lo_offset = rr["lo_offset"]
    by_name = {r.name: r for r in radios}

    carriers, services, inputs = [], [], []
    used_sid, used_lcn = {}, {}
    next_lcn = 1
    dflt = cfg["defaults"]
    for ci, c in enumerate(cfg["carrier"]):
        cc = dict(CARRIER_DEFAULTS, **c)
        std = cc["standard"].lower()
        if "channel" in cc and "frequency" not in cc:
            cc["frequency"] = channel_frequency(cc["channel"])
        freq = float(_num(cc.get("frequency", 0), f"carrier {ci} frequency"))
        keys = {"dvbt": ("bandwidth", "mode", "constellation", "code_rate", "guard"),
                "dvbc": ("constellation", "symbol_rate", "rolloff"),
                "dvbs": ("code_rate", "symbol_rate", "rolloff", "polarization", "orbital"),
                "dvbt2": ("bandwidth", "fft", "extended", "guard", "pilot", "constellation", "rotation", "code_rate",
                          "frame", "fec_blocks", "data_symbols", "ti_blocks", "l1_constellation", "cell_id",
                          "network_id", "t2_system_id", "t2_frames"),
                "dvbs2": ("modulation", "code_rate", "symbol_rate", "rolloff", "pilots", "frame", "gold_code",
                          "polarization", "orbital", "lnb_lo"),
                "ts": ("ts_rate", "bandwidth", "mode", "constellation", "code_rate", "guard")}.get(std, ())
        if std == "dvbt2":                       # DVB-T defaults do not apply
            for k, v in (("constellation", "256qam"), ("code_rate", "2/3"), ("guard", "1/128")):
                if k not in c:
                    cc[k] = v
        if std == "dvbc" and "constellation" not in c:
            cc["constellation"] = "256qam"
        if std in ("dvbs", "dvbs2") and "code_rate" not in c:
            cc["code_rate"] = "3/4"
        params = {k: cc[k] for k in keys if k in cc}
        if "bandwidth" in params:
            params["bandwidth"] = int(_num(params["bandwidth"], "bandwidth"))
        try:
            ts_rate = capacity.rate(std, **params)
        except (capacity.CapacityError, KeyError) as e:
            raise ConfigError(f"carrier {ci} ({cc.get('name', '')}): {e}") from e
        radio = None
        if std != "ts":
            rsel = cc.get("radio", 0)
            radio = by_name.get(rsel) if isinstance(rsel, str) else (radios[rsel] if isinstance(rsel, int) and rsel < len(radios) else None)
            if radio is None:
                raise ConfigError(f"carrier {ci}: radio {rsel!r} is not defined ([[radio]])")
            if std not in ("dvbt", "dvbt2", "dvbc", "dvbs", "dvbs2"):
                raise ConfigError(f"carrier {ci}: unknown standard {std} (dvbt, dvbt2, dvbc, dvbs, dvbs2, ts)")
            if std == "dvbs2":
                params.setdefault("symbol_rate", 27_500_000)
                params["symbol_rate"] = float(_num(params["symbol_rate"], "symbol_rate"))
                params.setdefault("rolloff", 0.35)
                params.setdefault("modulation", "8psk")
                params.setdefault("code_rate", "3/4")
                params.setdefault("pilots", True)
                params.setdefault("frame", "normal")
                if float(params["rolloff"]) not in (0.35, 0.25, 0.2):
                    raise ConfigError(f"carrier {ci}: DVB-S2 rolloff must be 0.35, 0.25 or 0.20")
                ts_rate = capacity.rate(std, **params)
            if std == "dvbs":
                params.setdefault("symbol_rate", 27_500_000)
                params["symbol_rate"] = float(_num(params["symbol_rate"], "symbol_rate"))
                params.setdefault("rolloff", 0.35)
                params.setdefault("code_rate", "3/4")
                ts_rate = capacity.rate(std, **params)
            if std == "dvbc":
                params.setdefault("symbol_rate", 6_900_000)
                params["symbol_rate"] = float(_num(params["symbol_rate"], "symbol_rate"))
                params.setdefault("rolloff", 0.15)
                if params.get("constellation") not in ("16qam", "64qam", "256qam"):
                    raise ConfigError(f"carrier {ci}: DVB-C constellation must be 16qam, 64qam or 256qam")
                ts_rate = capacity.rate(std, **params)
            if freq <= 0:
                raise ConfigError(f"carrier {ci}: frequency (or channel) is required")
        tsid = int(cc.get("tsid", ci + 1))
        car = Carrier(ci, str(cc.get("name", f"Mux {ci + 1}")), radio, std, freq, params, tsid,
                      int(cc.get("onid", net["onid"])), ts_rate)
        car.cell_id = int(cc["cell_id"])
        car.gain_db = float(cc["gain_db"])
        car.si_reserve = float(cc["si_reserve"])
        mon = cc["monitor"]
        car.monitor = [mon] if isinstance(mon, str) else list(mon)
        car.output = str(cc.get("output", ""))
        if std == "ts" and not car.output:
            raise ConfigError(f"carrier {ci}: standard 'ts' needs output = \"udp://host:port\" or a file")
        if radio:
            radio.carriers.append(car)
        carriers.append(car)

        for si, s in enumerate(cc.get("service", [])):
            if "source" not in s:
                raise ConfigError(f"carrier {ci} service {si}: 'source' is required")
            opts = copy.deepcopy(dflt)
            for k, v in s.items():
                if k == "encode" and isinstance(v, dict):
                    opts["encode"] = dict(opts["encode"], **v)
                    opts["encode_on"] = True
                elif k == "encode" and v != "auto":
                    opts["encode_on"] = bool(v)
                else:
                    opts[k] = v
            sid = int(s.get("sid", (tsid << 8) | (si + 1)) if not isinstance(s.get("sid"), str) else int(s["sid"], 0))
            if sid in used_sid:
                raise ConfigError(f"service_id 0x{sid:04X} used twice ({used_sid[sid]} and {s.get('name')})")
            lcn = s.get("lcn")
            if lcn is None:
                while next_lcn in used_lcn:
                    next_lcn += 1
                lcn = next_lcn
            lcn = int(lcn)
            if lcn in used_lcn:
                raise ConfigError(f"LCN {lcn} used twice ({used_lcn[lcn]} and {s.get('name')})")
            if not 0 < lcn < 1024:
                raise ConfigError(f"LCN {lcn}: must be 1..1023")
            name = str(s.get("name", f"Service {len(services) + 1}"))
            if not re.fullmatch(r"[^\"\\\x00-\x1f]{1,60}", name):
                raise ConfigError(f"service name {name!r}: 1-60 characters, no quotes or backslashes")
            used_sid[sid] = name
            used_lcn[lcn] = name
            stype = s.get("type", "auto")
            if isinstance(stype, str) and stype != "auto":
                if stype not in __import__("dvbtx.si", fromlist=["SERVICE_TYPES"]).SERVICE_TYPES:
                    raise ConfigError(f"service {name}: unknown type {stype}")
            pid_base = int(s.get("pid_base", 0x100 + si * 0x20))
            if not 0x20 <= pid_base <= 0x1FC0:
                raise ConfigError(f"service {name}: pid_base out of range")
            backup = opts.get("backup")
            backup = "" if backup is False else str(backup or "")
            svc = Service(len(services), car, si, name, str(s.get("provider", net["provider"])), sid, lcn,
                          bool(s.get("visible", True)), stype, str(s["source"]), backup, str(s.get("epg_id", "")),
                          pid_base, opts)
            car.services.append(svc)
            services.append(svc)

    # inputs: one per primary source, one per distinct backup source
    port = int(cfg["ingest"]["base_port"])
    backups = {}
    for svc in services:
        kind, addr = _input_kind(svc.source, svc.opts)
        if kind == "ffmpeg":
            addr = f"127.0.0.1:{port + len(inputs)}"
        inp = Input(len(inputs), f"{svc.name}", addr, svc.source, kind, svc, "primary", svc.opts)
        inputs.append(inp)
        svc.input = inp
    for svc in services:
        if not svc.backup:
            continue
        key = svc.backup
        if key not in backups:
            kind, addr = _input_kind(svc.backup, svc.opts)
            if kind == "ffmpeg":
                addr = f"127.0.0.1:{port + len(inputs)}"
            bopts = dict(svc.opts, loop=True, encode_on=bool(svc.opts.get("backup_encode", False)),
                         ffmpeg_input=[], ffmpeg_output=[], readrate="auto")
            bname = re.sub(r'["\\\x00-\x1f]', "_", os.path.basename(svc.backup))[:50]
            inp = Input(len(inputs), f"backup:{bname}", addr, svc.backup, kind, None,
                        "backup", bopts)
            inputs.append(inp)
            backups[key] = inp
        svc.backup_input = backups[key]

    for r in radios:
        if r.carriers:
            _plan_radio(r)

    # capacity sanity of fixed encoder bitrates
    for car in carriers:
        fixed = sum(_bitrate(s.opts["encode"].get("video_bitrate")) + _bitrate(s.opts["encode"].get("audio_bitrate"))
                    for s in car.services if s.opts.get("encode_on") and s.opts["encode"].get("video_bitrate") != "auto")
        if fixed > car.ts_rate * (1 - car.si_reserve):
            raise ConfigError(f"carrier {car.name}: encoder bitrates ({fixed / 1e6:.2f} Mbit/s) exceed the "
                              f"channel capacity ({car.ts_rate / 1e6:.2f} Mbit/s)")
    return Plan(cfg, radios, carriers, services, inputs, tz)


def _bitrate(v):
    if v in (None, "", "auto"):
        return 0
    if isinstance(v, (int, float)):
        return float(v)
    p = parse_value(str(v))
    return float(p) if isinstance(p, (int, float)) else 0


def auto_video_bitrate(svc):
    """share of the carrier capacity for an encoder with video_bitrate = auto"""
    car = svc.carrier
    budget = car.ts_rate * (1 - car.si_reserve) * 0.97
    fixed, nauto = 0.0, 0
    for s in car.services:
        enc = s.opts["encode"]
        is_enc = s.opts.get("encode_on") or _needs_encode(s.source)
        if not is_enc:
            fixed += 6e6         # copied stream of unknown rate: assume a typical HD channel
            continue
        fixed += _bitrate(enc.get("audio_bitrate")) + 20e3
        if enc.get("video_bitrate") in (None, "", "auto"):
            nauto += 1
        else:
            fixed += _bitrate(enc.get("video_bitrate"))
    cap = _bitrate(svc.opts["encode"].get("max_auto_bitrate", "8M")) or 8e6
    return min(cap, max(500e3, (budget - fixed) / max(1, nauto)))


def _needs_encode(src):
    return bool(re.match(r"^(v4l2|screen|x11|kms|alsa|pulse|lavfi|testsrc):", src))


def _input_kind(src, opts):
    if opts.get("direct") or src.startswith("udp+direct://") or src.startswith("direct://"):
        addr = re.sub(r"^(udp\+direct|direct)://", "", src)
        addr = addr.replace("udp://", "")
        return "direct", addr
    return "ffmpeg", ""


def _plan_radio(r):
    cars = r.carriers
    ofdm = [c for c in cars if c.standard == "dvbt"]
    if ofdm:
        ref = ofdm[0].params
        for c in ofdm[1:]:
            for k in ("mode", "guard", "bandwidth"):
                if c.params.get(k) != ref.get(k):
                    raise ConfigError(f"radio {r.name}: DVB-T carriers {ofdm[0].name} and {c.name} differ in {k}; "
                                      "DVB-T carriers sharing one radio need the same mode, guard interval and bandwidth")
        bw = int(ref.get("bandwidth", 8_000_000))
        spacing = capacity.dvbt_spacing(bw, ref.get("mode", "8k"))
        fft = capacity.DVBT_MODES[ref.get("mode", "8k")]
        f0 = min(c.frequency for c in ofdm)
        for c in ofdm:
            d = (c.frequency - f0) / spacing
            if abs(d - round(d)) > 1e-3:
                raise ConfigError(f"radio {r.name}: {c.name} at {c.frequency / 1e6:.6f} MHz is not on the "
                                  f"{spacing:.4f} Hz grid of {f0 / 1e6:.6f} MHz")
    spans = sorted((c.frequency - occupied_half(c), c.frequency + occupied_half(c), c) for c in cars)
    for (a0, a1, ca), (b0, b1, cb) in zip(spans, spans[1:]):
        if b0 < a1 + 50e3:
            raise ConfigError(f"radio {r.name}: carriers {ca.name} and {cb.name} overlap")
    lo_edge, hi_edge = spans[0][0], max(s[1] for s in spans)
    if r.center <= 0:
        if len(cars) == 1:
            c = cars[0]
            lo = occupied_half(c) * 1.05 if r.lo_offset == "auto" else float(_num(r.lo_offset, "lo_offset"))
            target = c.frequency - lo             # keep the LO leakage out of the channel
        else:
            target = (lo_edge + hi_edge) / 2
        r.center = f0 + round((target - f0) / spacing) * spacing if ofdm else round(target / 1000) * 1000
    elif ofdm:
        d = (f0 - r.center) / spacing
        if abs(d - round(d)) > 1e-3:
            raise ConfigError(f"radio {r.name}: center must differ from the DVB-T carriers by a multiple of {spacing:.4f} Hz")
    for c in cars:
        c.offset = round((c.frequency - r.center) / spacing) * spacing if c.standard == "dvbt" else c.frequency - r.center
    edge = max(abs(c.offset) + occupied_half(c) for c in cars)
    need = 2 * (edge + 150e3)
    lo_rate, hi_rate = DRIVER_RATES.get(r.driver, (0, 1e12))
    if r.driver == "hackrf" and r.bandwidth <= 0:
        # analog filter wide enough for the band edges (a filter cutting them distorts single-carrier
        # signals: DVB-C 256-QAM errors), and a rate that keeps the DAC images out of it
        filt = next((f for f in HACKRF_FILTERS if f / 2 >= edge + 100e3), HACKRF_FILTERS[-1])
        r.bandwidth = filt
        need = max(need, min(hi_rate, edge + filt / 2 + 300e3))
    if ofdm:
        gi_den = max(int(c.params.get("guard", "1/32").split("/")[1]) for c in ofdm)
        if r.rate <= 0:
            nfft = _smooth_size(max(need, lo_rate) / spacing, fft)   # integer oversampling of the native rate
            r.rate = nfft * spacing
        else:
            nfft = round(r.rate / spacing)
            if abs(nfft * spacing - r.rate) > 1e-6 * r.rate:
                raise ConfigError(f"radio {r.name}: rate {r.rate:.0f} is not a multiple of {spacing:.6f} Hz "
                                  f"(for instance {_smooth_size(r.rate / spacing, 64) * spacing:.3f})")
        if nfft % gi_den:
            raise ConfigError(f"radio {r.name}: FFT size {nfft} not divisible by the guard interval")
        r.nfft = nfft
    elif r.rate <= 0:
        r.rate = max(lo_rate, math.ceil(need / 100e3) * 100e3)
    if r.rate < need:
        raise ConfigError(f"radio {r.name}: rate {r.rate / 1e6:.3f} MS/s too low for the carriers (needs {need / 1e6:.3f})")
    if r.rate > hi_rate * 1.0001:
        raise ConfigError(f"radio {r.name}: {len(cars)} carriers need {r.rate / 1e6:.3f} MS/s, more than "
                          f"{r.driver} can do ({hi_rate / 1e6:.2f}); split them over several radios")


def write_engine_ini(plan, path, si_dir):
    c = plan.cfg
    ing, eng, epg = c["ingest"], c["engine"], c["epg"]
    L = ["# generated by dvbtx - do not edit", "[engine]", f"log_level = {eng['log_level']}",
         f"stats_interval_ms = {eng['stats_interval_ms']}", ""]
    for inp in plan.inputs:
        o = inp.options
        L += [f"[input {inp.index}]", f"name = {inp.name[:60]}", f"addr = {inp.addr}",
              f"program = {int(o.get('program', 0))}", f"buffer_ms = {int(o['buffer_ms'])}",
              f"ring_pkts = {int(o['ring_pkts'])}", ""]
    for r in plan.radios:
        if not r.carriers:
            continue
        L += [f"[radio {r.index}]", f"driver = {r.driver}", f"args = {r.args}", f"freq = {r.center:.3f}",
              f"rate = {r.rate:.6f}", f"bandwidth = {r.bandwidth:.0f}", f"gain = {r.gain}", f"amp = {int(r.amp)}",
              f"ppm = {r.ppm}", f"buffer_ms = {r.buffer_ms}", f"backoff_db = {r.backoff_db}", f"clock = {r.clock}", ""]
    mon_base = int(ing["monitor_base_port"])
    pf_ms, sch_ms = int(epg["pf_cycle_ms"]), int(epg["schedule_cycle_ms"])
    net = c["network"]
    for car in plan.carriers:
        p = car.params
        L += [f"[carrier {car.index}]", f"standard = {car.standard}", f"tsid = {car.tsid}"]
        if car.standard == "dvbt2":
            L += [f"radio = {car.radio.index}", f"offset = {car.offset:.3f}", f"frequency = {car.frequency:.0f}",
                  f"gain_db = {car.gain_db}"]
            for k, v in car.params.items():
                L.append(f"{k} = {int(v) if isinstance(v, bool) else v}")
        elif car.standard == "dvbs2":
            L += [f"radio = {car.radio.index}", f"offset = {car.offset:.3f}", f"symbol_rate = {p['symbol_rate']:.1f}",
                  f"modulation = {p['modulation']}", f"code_rate = {p['code_rate']}", f"frame = {p['frame']}",
                  f"pilots = {int(bool(p['pilots']))}", f"rolloff = {p['rolloff']}",
                  f"gold_code = {int(car.params.get('gold_code', 0))}", f"gain_db = {car.gain_db}"]
        elif car.standard == "dvbs":
            L += [f"radio = {car.radio.index}", f"offset = {car.offset:.3f}", f"symbol_rate = {p['symbol_rate']:.1f}",
                  f"code_rate = {p['code_rate']}", f"rolloff = {p.get('rolloff', 0.35)}", f"gain_db = {car.gain_db}"]
        elif car.standard == "dvbc":
            L += [f"radio = {car.radio.index}", f"offset = {car.offset:.3f}", f"symbol_rate = {p['symbol_rate']:.1f}",
                  f"constellation = {p['constellation']}", f"rolloff = {p.get('rolloff', 0.15)}",
                  f"gain_db = {car.gain_db}"]
        elif car.standard == "dvbt":
            L += [f"radio = {car.radio.index}", f"offset = {car.offset:.3f}", f"mode = {p.get('mode', '8k')}",
                  f"constellation = {p.get('constellation', '64qam')}", f"code_rate = {p.get('code_rate', '7/8')}",
                  f"guard = {p.get('guard', '1/32').split('/')[1]}", f"bandwidth = {p.get('bandwidth', 8000000)}",
                  f"cell_id = {car.cell_id}", f"gain_db = {car.gain_db}"]
        else:
            L += [f"ts_rate = {car.ts_rate:.1f}", f"output = {car.output}"]
        mons = [f"udp://127.0.0.1:{mon_base + car.index}"] + car.monitor
        L.append(f"monitor = {','.join(mons)}")
        sd = si_dir
        L += [f"table = 0x10,{net['nit_period_ms']},{sd}/c{car.index}_nit.sec",
              f"table = 0x11,{net['sdt_period_ms']},{sd}/c{car.index}_sdt.sec",
              f"table = 0x12,{pf_ms},{sd}/c{car.index}_eit_pf.sec"]
        if epg.get("schedule", True):
            L.append(f"table = 0x12,{sch_ms},{sd}/c{car.index}_eit_sch.sec")
        L += [f"tot_desc = {sd}/tot_desc.bin", "nit_pid = 16", ""]
    for s in plan.services:
        o = s.opts
        L += [f"[service {s.index}]", f"carrier = {s.carrier.index}", f"name = {s.name}", f"sid = {s.sid}",
              f"pid_base = {s.pid_base}", f"primary = {s.input.index}",
              f"backup = {s.backup_input.index if s.backup_input else -1}",
              f"switch_timeout_ms = {int(o['switch_timeout_ms'])}", f"return_ms = {int(o['return_ms'])}",
              f"max_late_ms = {int(o['max_late_ms'])}", f"early_ms = {int(o['early_ms'])}",
              f"pcr_max_ms = {int(o['pcr_max_ms'])}", f"pmt_period_ms = {int(o['pmt_period_ms'])}",
              f"priority = {int(o.get('priority', 5))}", ""]
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write("\n".join(L))
    os.replace(tmp, path)


def describe(plan):
    """Human summary of the plan (dvbtx check)."""
    out = []
    for r in plan.radios:
        if not r.carriers:
            continue
        out.append(f"radio {r.name} ({r.driver}{' ' + r.args if r.args else ''}): LO {r.center / 1e6:.4f} MHz, "
                   f"{r.rate / 1e6:.6f} MS/s{f' (FFT {r.nfft})' if r.nfft else ''}"
                   f"{f', filter {r.bandwidth / 1e6:g} MHz' if r.bandwidth else ''}, gain {r.gain:g} dB{' +amp' if r.amp else ''}")
    for c in plan.carriers:
        p = c.params
        where = f"{c.frequency / 1e6:.3f} MHz (offset {c.offset / 1e6:+.3f})" if c.radio else f"-> {c.output}"
        if c.standard == "dvbt":
            desc_ = (f"DVB-T {p.get('mode')} {p.get('constellation')} {p.get('code_rate')} GI {p.get('guard')} "
                     f"{int(p.get('bandwidth', 0)) // 1000000} MHz")
        elif c.standard == "dvbc":
            desc_ = f"DVB-C {p['constellation']} {p['symbol_rate'] / 1e6:g} Msym/s rolloff {p.get('rolloff', 0.15)}"
        elif c.standard == "dvbt2":
            desc_ = (f"DVB-T2 {p.get('fft', '32k')}{' ext' if p.get('extended', True) else ''} {p.get('constellation')} "
                     f"{p.get('code_rate')} GI {p.get('guard')} {p.get('pilot', 'PP7')} "
                     f"{float(p.get('bandwidth', 8e6)) / 1e6:g} MHz")
        elif c.standard == "dvbs2":
            desc_ = (f"DVB-S2 {p['modulation']} {p['code_rate']} {p['symbol_rate'] / 1e6:g} Msym/s rolloff {p['rolloff']} "
                     f"{p['frame']} frames, pilots {'on' if p['pilots'] else 'off'}")
        elif c.standard == "dvbs":
            desc_ = f"DVB-S QPSK {p['code_rate']} {p['symbol_rate'] / 1e6:g} Msym/s rolloff {p.get('rolloff', 0.35)}"
        else:
            desc_ = c.standard
        out.append(f"carrier {c.index} {c.name!r}: {desc_}, {where}, TSID {c.tsid}, useful rate "
                   f"{c.ts_rate / 1e6:.3f} Mbit/s, {len(c.services)} service(s)")
        for s in c.services:
            b = f", backup {s.backup}" if s.backup else ""
            out.append(f"   LCN {s.lcn:3d}  SID 0x{s.sid:04X}  {s.name:<24} {s.source}{b}")
    return "\n".join(out)
