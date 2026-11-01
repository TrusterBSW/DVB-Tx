"""Supervisor: SI tables, the C engine, the ingest processes, TSDuck analysers, alerts, history."""

import collections
import copy
import datetime as dt
import hashlib
import json
import logging
import os
import queue
import re
import shutil
import signal
import socket
import subprocess
import threading
import time
import tomllib
import urllib.request

from . import channels as chmod
from . import confedit
from . import config as cfgmod
from . import epg as epgmod
from . import gpu
from . import si
from .ingest import IngestProc

log = logging.getLogger("dvbtx")
UTC = dt.timezone.utc


def find_engine(cfg):
    cands = [cfg["engine"].get("binary"), os.environ.get("DVBTX_ENGINE"),
             os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "engine", "build", "dvbtx-engine"),
             "/usr/lib/dvbtx/dvbtx-engine"]
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    raise FileNotFoundError("dvbtx-engine not found (build it with 'make -C engine' or set engine.binary)")


def _write_atomic(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


class Versions:
    """version_number per sub-table, bumped only when its content changes"""

    def __init__(self):
        self.d = {}

    def get(self, key, digest):
        old = self.d.get(key)
        if old is None:
            v = int(digest[:2], 16) & 0x1F       # random-ish start: a restart is seen as a change
        elif old[0] == digest:
            return old[1]
        else:
            v = (old[1] + 1) & 0x1F
        self.d[key] = (digest, v)
        return v


def _digest(sections):
    h = hashlib.sha1()
    for s in sections:
        h.update(s[:5] + s[6:-4])               # without version byte and CRC
    return h.hexdigest()


class SiManager:
    def __init__(self, plan, si_dir, store, detected):
        self.plan = plan
        self.dir = si_dir
        self.store = store
        self.detected = detected                  # sid -> service_type guessed from the streams
        self.ver = Versions()
        self.next_pf = None
        self.next_sched = 0
        self.next_tot = 0
        self.net = plan.cfg["network"]
        self.charset = self.net["charset"]
        self.last_static = None
        self.stats = {}

    def _stype(self, s):
        if isinstance(s.type, int):
            return s.type
        if isinstance(s.type, str) and s.type != "auto":
            return si.SERVICE_TYPES[s.type]
        return self.detected.get(s.sid, 0x19)

    def _delivery(self, car):
        p = car.params
        if car.standard == "dvbc":
            return si.cable_delivery(car.frequency, p["symbol_rate"], p["constellation"])
        if car.standard == "dvbt2":
            return si.t2_delivery(int(p.get("t2_system_id", 0x8001)), car.frequency, float(p.get("bandwidth", 8e6)),
                                  p.get("guard", "1/128"), p.get("fft", "32k"), car.cell_id,
                                  unspecified=not self.net["frequency_in_nit"])
        if car.standard == "dvbs2":
            return si.satellite_delivery(car.frequency + float(p.get("lnb_lo", 9_750_000_000)), p["symbol_rate"],
                                         p["code_rate"], p.get("polarization", "h"), True,
                                         p["modulation"] if p["modulation"] in ("qpsk", "8psk") else "auto",
                                         float(p.get("rolloff", 0.35)), float(p.get("orbital", 19.2)))
        if car.standard == "dvbs":
            # the frequency in the NIT is the satellite one: the L-band IF + the LNB oscillator
            return si.satellite_delivery(car.frequency + float(p.get("lnb_lo", 9_750_000_000)), p["symbol_rate"],
                                         p["code_rate"], p.get("polarization", "h"), False, "qpsk",
                                         float(p.get("rolloff", 0.35)), float(p.get("orbital", 19.2)))
        if car.standard == "dvbt" or (car.standard == "ts" and car.frequency and "constellation" in p):
            return si.terrestrial_delivery(car.frequency, p.get("bandwidth", 8_000_000), p.get("constellation", "64qam"),
                                           p.get("code_rate", "7/8"), p.get("guard", "1/32"), p.get("mode", "8k"),
                                           unspecified=not self.net["frequency_in_nit"])
        return b""

    def write_static(self):
        plan = self.plan
        ts_list = []
        for car in plan.carriers:
            ts_list.append({"tsid": car.tsid, "onid": car.onid, "delivery": self._delivery(car),
                            "services": [(s.sid, self._stype(s), s.lcn, s.visible) for s in car.services]})
        nit0 = si.nit_sections(self.net["network_id"], self.net["name"], ts_list, 0, self.charset, self.net["lcn_pds"])
        # the names are in the SDT, but many TVs only look for changes when the NIT version changes:
        # a rename (or a new provider name) bumps it too
        names = hashlib.sha1("|".join(f"{s.sid}:{s.name}:{s.provider}" for s in plan.services).encode()).hexdigest()
        v = self.ver.get("nit", _digest(nit0) + names)
        nit = si.nit_sections(self.net["network_id"], self.net["name"], ts_list, v, self.charset, self.net["lcn_pds"])
        nit_b = b"".join(nit)
        for car in plan.carriers:
            _write_atomic(os.path.join(self.dir, f"c{car.index}_nit.sec"), nit_b)
            svcs = [{"sid": s.sid, "type": self._stype(s), "name": s.name, "provider": s.provider,
                     "eit_schedule": bool(self.store and self.store.events(s.sid)), "eit_pf": True}
                    for s in car.services]
            sdt0 = si.sdt_sections(car.tsid, car.onid, svcs, 0, self.charset)
            v = self.ver.get(("sdt", car.index), _digest(sdt0))
            sdt = si.sdt_sections(car.tsid, car.onid, svcs, v, self.charset)
            if self.net.get("sdt_other"):
                for other in plan.carriers:
                    if other is car:
                        continue
                    osv = [{"sid": s.sid, "type": self._stype(s), "name": s.name, "provider": s.provider}
                           for s in other.services]
                    o0 = si.sdt_sections(other.tsid, other.onid, osv, 0, self.charset, actual=False)
                    ov = self.ver.get(("sdto", car.index, other.index), _digest(o0))
                    sdt += si.sdt_sections(other.tsid, other.onid, osv, ov, self.charset, actual=False)
            _write_atomic(os.path.join(self.dir, f"c{car.index}_sdt.sec"), b"".join(sdt))
        self.stats["nit_bytes"] = len(nit_b)

    def write_tot(self):
        try:
            d = si.local_time_offset_descriptor(self.net["country"], self.net["timezone"])
        except Exception as e:                       # noqa: BLE001 - bad zone data must not stop the TOT
            log.warning("TOT: %s", e)
            d = b""
        _write_atomic(os.path.join(self.dir, "tot_desc.bin"), d)

    def write_pf(self, now):
        nxt = None
        for car in self.plan.carriers:
            secs = []
            for s in car.services:
                evs = self.store.events(s.sid) if self.store else []
                pres, fol, boundary = epgmod.present_following(evs, now)
                if boundary and (nxt is None or boundary < nxt):
                    nxt = boundary
                s0 = si.eit_pf(s.sid, car.tsid, car.onid, pres, fol, 0, self.charset)
                v = self.ver.get(("pf", s.sid), _digest(s0))
                secs += si.eit_pf(s.sid, car.tsid, car.onid, pres, fol, v, self.charset)
            _write_atomic(os.path.join(self.dir, f"c{car.index}_eit_pf.sec"), b"".join(secs))
        self.next_pf = nxt

    def write_schedule(self, now):
        days = int(self.plan.cfg["epg"]["days"])
        total = 0
        for car in self.plan.carriers:
            secs = []
            for s in car.services:
                evs = self.store.events(s.sid) if self.store else []
                if not evs:
                    continue
                s0 = si.eit_schedule(s.sid, car.tsid, car.onid, evs, now, days, lambda t: 0, self.charset)
                v = self.ver.get(("sch", s.sid), _digest(s0))
                secs += si.eit_schedule(s.sid, car.tsid, car.onid, evs, now, days, lambda t, v=v: v, self.charset)
            data = b"".join(secs)
            total += len(data)
            _write_atomic(os.path.join(self.dir, f"c{car.index}_eit_sch.sec"), data)
            cyc = float(self.plan.cfg["epg"]["schedule_cycle_ms"]) / 1000
            self.stats[f"eit_sch_rate_{car.index}"] = len(data) * 8 * 1.03 / cyc
        self.stats["eit_sch_bytes"] = total

    def tick(self, force=False):
        now = dt.datetime.now(UTC)
        t = time.time()
        epg_changed = False
        if self.store and (force or t >= self.next_sched):
            try:
                epg_changed = self.store.refresh(force)
            except Exception as e:                   # noqa: BLE001 - a broken EPG file must not stop the TV
                log.warning("EPG refresh: %s", e)
        static = repr(sorted((sid, v) for sid, v in self.detected.items()))
        if force or epg_changed or static != self.last_static:
            self.last_static = static
            self.write_static()
        if force or epg_changed or (self.next_pf and now >= self.next_pf):
            self.write_pf(now)
        if force or epg_changed or t >= self.next_sched:
            if self.plan.cfg["epg"].get("schedule", True):
                self.write_schedule(now)
            self.next_sched = t + 600           # also drops past events every 10 min
        if force or t >= self.next_tot:
            self.write_tot()
            self.next_tot = t + 3600


class Exporter:
    """Sends JSON documents to udp://host:port or tcp://host:port (one line each)."""

    def __init__(self, urls):
        self.targets = []
        for u in urls or []:
            proto, _, hp = u.partition("://")
            host, _, port = hp.rpartition(":")
            if proto not in ("udp", "tcp") or not port.isdigit():
                log.warning("export target %r ignored (udp://host:port or tcp://host:port)", u)
                continue
            self.targets.append([proto, host, int(port), None])
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def send(self, doc):
        if not self.targets:
            return
        data = (doc if isinstance(doc, str) else json.dumps(doc, separators=(",", ":"))).encode() + b"\n"
        for t in self.targets:
            try:
                if t[0] == "udp":
                    self.udp.sendto(data[:65000], (t[1], t[2]))
                else:
                    if t[3] is None:
                        t[3] = socket.create_connection((t[1], t[2]), timeout=2)
                    t[3].sendall(data)
            except OSError:
                if t[3]:
                    t[3].close()
                t[3] = None


class Analyzer:
    """TSDuck 'tsp -P analyze' on the monitor copy of one carrier, JSON report every interval."""

    def __init__(self, car, port, interval, tsp, exporter):
        self.car = car
        self.port = port
        self.interval = interval
        self.tsp = tsp
        self.exporter = exporter
        self.proc = None
        self.report = None
        self.at = 0
        self.lines = collections.deque(maxlen=30)
        self.next_start = 0

    def start(self):
        cmd = [self.tsp, "--realtime", "-I", "ip", "--buffer-size", "16000000", str(self.port),
               "-P", "continuity", "-P", "analyze", "--interval", str(self.interval), "--json-line=DVBTXJSON",
               "-O", "drop"]
        try:
            from .ingest import _die_with_parent
            self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                         preexec_fn=_die_with_parent)
        except OSError as e:
            log.warning("tsduck analyser: %s", e)
            self.next_start = time.monotonic() + 30
            return
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in iter(self.proc.stderr.readline, b""):
            line = raw.decode("utf-8", "replace").strip()
            i = line.find("DVBTXJSON")
            if i >= 0:
                try:
                    rep = json.loads(line[i + 9:])
                except ValueError:
                    continue
                self.report = rep
                self.at = time.time()
                self.exporter.send({"type": "tsduck_analyze", "carrier": self.car.index, "carrier_name": self.car.name,
                                    "time": self.at, "report": rep})
            elif line:
                self.lines.append(line)

    def poll(self):
        if self.proc is None:
            if time.monotonic() >= self.next_start:
                self.start()
        elif self.proc.poll() is not None:
            self.proc = None
            self.next_start = time.monotonic() + 5

    def stop(self):
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
                self.proc.wait(3)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                pass

    def summary(self):
        r = self.report
        if not r:
            return None
        ts = r.get("ts", {})
        pk = ts.get("packets", {})
        return {"age_s": round(time.time() - self.at, 1), "bitrate": ts.get("bitrate"),
                "packets": pk.get("total"), "invalid_syncs": pk.get("invalid-syncs"),
                "transport_errors": pk.get("transport-errors"), "suspect_ignored": pk.get("suspect-ignored"),
                "unreferenced_pids": ts.get("pids", {}).get("unreferenced"),
                "services": [{"id": s.get("id"), "name": s.get("name"), "bitrate": s.get("bitrate")}
                             for s in r.get("services", [])],
                "pids": [{"id": p.get("id"), "description": p.get("description"), "bitrate": p.get("bitrate"),
                          "discontinuities": p.get("packets", {}).get("discontinuities"),
                          "duplicated": p.get("packets", {}).get("duplicated")}
                         for p in r.get("pids", [])],
                "log": list(self.lines)[-5:]}


class Alerts:
    def __init__(self, cfg):
        self.cfg = cfg
        self.active = {}            # key -> dict
        self.history = collections.deque(maxlen=200)
        self.last_notified = {}
        self.pending = {}           # key -> first time seen: a condition must hold hold_s to be raised
        self.hold_s = float(cfg.get("hold_s", 4))

    def evaluate(self, conds):
        """conds: dict key -> (level, message) currently true"""
        now = time.time()
        for k in list(self.pending):
            if k not in conds:
                del self.pending[k]
        for k, (lvl, msg) in conds.items():
            a = self.active.get(k)
            if a is None and not k.startswith("underrun."):
                first = self.pending.setdefault(k, now)
                if now - first < self.hold_s:
                    continue
            if a is None:
                a = {"key": k, "level": lvl, "message": msg, "since": now}
                self.active[k] = a
                self._notify("raise", a)
            else:
                a["message"] = msg
                if lvl != a["level"]:
                    a["level"] = lvl
                    self._notify("raise", a)
                elif now - self.last_notified.get(k, 0) > float(self.cfg["repeat_s"]):
                    self._notify("repeat", a)
        for k in list(self.active):
            if k not in conds:
                a = self.active.pop(k)
                a["until"] = now
                self._notify("clear", a)

    def _notify(self, event, a):
        self.last_notified[a["key"]] = time.time()
        rec = dict(a, event=event, time=time.time())
        self.history.appendleft(rec)
        fn = log.warning if a["level"] != "info" and event != "clear" else log.info
        fn("alert %s [%s] %s", event, a["level"], a["message"])
        hook, cmd = self.cfg.get("webhook"), self.cfg.get("command")
        if hook:
            def post():
                try:
                    req = urllib.request.Request(hook, data=json.dumps(rec).encode(), headers={"Content-Type": "application/json"})
                    urllib.request.urlopen(req, timeout=5).close()
                except OSError as e:
                    log.debug("webhook: %s", e)
            threading.Thread(target=post, daemon=True).start()
        if cmd:
            env = dict(os.environ, DVBTX_ALERT_EVENT=event, DVBTX_ALERT_LEVEL=a["level"], DVBTX_ALERT_KEY=a["key"],
                       DVBTX_ALERT_MESSAGE=a["message"])
            try:
                subprocess.Popen(cmd, shell=True, env=env, stdin=subprocess.DEVNULL)
            except OSError as e:
                log.debug("alert command: %s", e)


class RateHistory:
    """the health of each channel, one value a second over 24 h: output bit rate, delay of the sending,
    video decoding margin, and the input's continuity errors (a counter); a ring of float32 per channel
    and measure (340 kB a day each) and one of times shared by all"""
    METRICS = ("rate", "late", "margin", "cc")

    def __init__(self, seconds=86400):
        import array
        self.n = seconds
        self._array = array.array
        self.times = array.array("d", [0.0]) * seconds
        self.data = {}                  # (sid, metric) -> array
        self.last = 0.0

    def add(self, t, values):
        """values: {sid: {metric: value or None}}"""
        slot = int(t) % self.n
        self.times[slot] = t
        self.last = t
        nan = float("nan")
        for sid, ms in values.items():
            for m in self.METRICS:
                a = self.data.get((sid, m))
                if a is None:
                    a = self.data[(sid, m)] = self._array("f", [nan]) * self.n
                v = ms.get(m)
                a[slot] = nan if v is None else v
        for (sid, m), a in self.data.items():
            if sid not in values:
                a[slot] = nan

    def query(self, sid, seconds, points=360):
        """per measure: bucket times and mean/min/max (cc: errors counted in each bucket), and statistics"""
        if (sid, "rate") not in self.data or not self.last:
            return None
        lo = self.last - seconds
        idx = sorted((self.times[i], i) for i in range(self.n) if self.times[i] > lo)
        if not idx:
            return None
        width = max(1.0, seconds / points)
        out = {"from": idx[0][0], "to": idx[-1][0]}
        for m in self.METRICS:
            a = self.data[(sid, m)]
            samples = [(t, a[i]) for t, i in idx if a[i] == a[i]]
            if m == "cc":                # a counter: what it gained (a restart of the input sets it back)
                inc, prev = [], None
                for t, v in samples:
                    inc.append((t, max(0.0, v - prev) if prev is not None and v >= prev else 0.0))
                    prev = v
                samples = inc
            buckets = {}
            for t, v in samples:
                buckets.setdefault(int((t - lo) // width), []).append(v)
            keys = sorted(buckets)
            vals = sorted(v for _, v in samples)
            n = len(vals)
            if not n:
                out[m] = None
                continue

            def pct(q):
                return vals[min(n - 1, int(q * (n - 1) + 0.5))]
            out[m] = {"t": [lo + (k + 0.5) * width for k in keys],
                      "mean": [sum(buckets[k]) / len(buckets[k]) for k in keys],
                      "min": [min(buckets[k]) for k in keys], "max": [max(buckets[k]) for k in keys],
                      "sum": [sum(buckets[k]) for k in keys],
                      "stats": {"mean": sum(vals) / n, "min": vals[0], "max": vals[-1], "median": pct(0.5),
                                "p95": pct(0.95), "p5": pct(0.05), "total": sum(vals), "samples": n}}
        return out


class History:
    """last 30 minutes, one point per engine stats line"""

    def __init__(self, n=1800):
        self.n = n
        self.t = collections.deque(maxlen=n)
        self.series = {}

    def add(self, t, values):
        self.t.append(t)
        for k, v in values.items():
            d = self.series.get(k)
            if d is None:
                d = self.series[k] = collections.deque([None] * (len(self.t) - 1), maxlen=self.n)
            d.append(v)
        for k, d in self.series.items():
            if len(d) < len(self.t):
                d.append(None)

    def dump(self, last=None):
        t = list(self.t)
        cut = -last if last else 0
        return {"t": t[cut:], "series": {k: list(v)[cut:] for k, v in self.series.items()}}


class _Call:
    """a function run by the main loop for another thread (web API)"""

    def __init__(self, fn):
        self.fn, self.ev, self.result, self.error = fn, threading.Event(), None, None


class Supervisor:
    def __init__(self, plan, args=None):
        self.plan = plan
        self.cfg = plan.cfg
        self.args = args
        self.work = plan.work_dir
        self.si_dir = os.path.join(self.work, "si")
        os.makedirs(self.si_dir, exist_ok=True)
        self.engine_bin = find_engine(self.cfg)
        self.ini = os.path.join(self.work, "engine.ini")
        epg = self.cfg["epg"]
        self.store = epgmod.EpgStore(epg.get("dir"), epg.get("urls"), plan.services, plan.tz, int(epg["days"]),
                                     int(epg["url_refresh_s"])) if (epg.get("dir") or epg.get("urls")) else None
        self.detected = {}
        self.si = SiManager(plan, self.si_dir, self.store, self.detected)
        self.ingest = [IngestProc(i, plan) for i in plan.inputs]
        gpu.MANAGER = gpu.Manager(self.cfg["ingest"])
        from .sysload import SystemLoad
        cards = []
        self.sysload = SystemLoad(lambda: gpu.MANAGER.devices or cards or cards.extend(gpu.detect()) or cards)
        mon = self.cfg["monitoring"]
        self.exporter = Exporter(mon.get("export"))
        self.stats_exporter = Exporter(mon.get("stats_export"))
        self.analyzers = self._make_analyzers()
        self.alerts = Alerts(self.cfg["alerts"])
        self.history = History()
        self.rates = RateHistory()
        self.stats = None
        self.stats_at = 0
        self.engine = None
        self.engine_lines = collections.deque(maxlen=200)
        self.engine_restarts = 0
        self.engine_next = 0
        self.started = time.time()
        self._stop = False          # set from signal handlers: a plain flag, no lock (an Event can deadlock)
        self._reload = False
        self.lock = threading.Lock()
        self.cmd_lock = threading.Lock()
        from .preview import Previews
        self.previews = Previews(self)
        # channels changed while running
        self.calls = queue.Queue()
        self.next_input = max((i.index for i in plan.inputs), default=-1) + 1
        self.retiring = []          # [IngestProc, input id, deadline]: old sources kept until replaced
        self.seq = 0
        self.replies = {}
        self.reply_cv = threading.Condition()
        self.fo = {}                # service_id -> state of the choice among its sources (see _failover)
        self.fo_wait = {}           # service_id -> {source k: [retry interval, not before]} of sources left

        # the configuration file as running: edits from the web page are pending until applied
        self.applied_doc = self._file_doc()

    def _make_analyzers(self):
        mon = self.cfg["monitoring"]
        use_tsp = mon.get("tsduck", "auto")
        tsp = mon.get("tsp", "tsp")
        if use_tsp is True or (use_tsp == "auto" and shutil.which(tsp)):
            base = int(self.cfg["ingest"]["monitor_base_port"])
            return [Analyzer(c, base + c.index, int(mon["interval"]), tsp, self.exporter) for c in self.plan.carriers]
        if use_tsp is True:
            log.warning("tsduck requested but '%s' not found", tsp)
        return []

    def _file_doc(self):
        try:
            with open(self.plan.config_path, "rb") as f:
                return tomllib.load(f)
        except (OSError, TypeError, tomllib.TOMLDecodeError):
            return {}

    # ---------------------------------------------------------------- engine

    def start_engine(self):
        cfgmod.write_engine_ini(self.plan, self.ini, self.si_dir)
        cmd = [self.engine_bin, self.ini]
        log.info("starting engine: %s", " ".join(cmd))
        from .ingest import _die_with_parent
        self.engine = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       preexec_fn=_die_with_parent)
        threading.Thread(target=self._read_engine_out, args=(self.engine,), daemon=True).start()
        threading.Thread(target=self._read_engine_err, args=(self.engine,), daemon=True).start()

    def _read_engine_out(self, proc):
        for raw in iter(proc.stdout.readline, b""):
            try:
                st = json.loads(raw)
            except ValueError:
                continue
            if st.get("type") == "stats":
                self._on_stats(st)
            elif st.get("type") == "reply":
                with self.reply_cv:
                    self.replies[st.get("seq")] = st
                    self.reply_cv.notify_all()

    def _read_engine_err(self, proc):
        for raw in iter(proc.stderr.readline, b""):
            line = raw.decode("utf-8", "replace").rstrip()
            if not line:
                continue
            self.engine_lines.append(line)
            lvl = logging.WARNING if " WRN " in line else logging.ERROR if " ERR " in line else logging.INFO
            logging.getLogger("dvbtx.engine").log(lvl, "%s", line.split(" ", 2)[-1])

    def _on_stats(self, st):
        with self.lock:
            self.stats = st
            self.stats_at = time.time()
        vals = {}
        for c in st.get("carriers", []):
            i = c["carrier"]
            vals[f"c{i}.rate"] = round(c["rate"] - c["null_rate"])
            vals[f"c{i}.null"] = round(c["null_rate"])
            vals[f"c{i}.si"] = round(c["si_rate"])
            vals[f"c{i}.lead"] = c.get("clock_lead_ms")
            for s in c.get("services", []):
                k = f"s{i}.{s['sid']}"
                vals[k + ".rate"] = round(s["rate"])
                vals[k + ".vmargin"] = s.get("video_margin_min_ms")
                vals[k + ".amargin"] = s.get("audio_margin_min_ms")
                vals[k + ".late"] = s.get("late_max_ms")
                # service type from the streams
                vt = s.get("video_type", -1)
                if s.get("streams"):
                    t = si.service_type_from_streams(vt if vt >= 0 else None, s.get("audio_types") or [])
                    if self.detected.get(s["sid"]) != t:
                        self.detected[s["sid"]] = t
        for r in st.get("radios", []):
            vals[f"r{r['index']}.cpu"] = r.get("cpu")
        self.history.add(st.get("time", time.time()), vals)
        cc = {i["index"]: i.get("cc_errors") for i in st.get("inputs", [])}
        prim = {sv.sid: sv.input.index for sv in self.plan.services if sv.input}
        self.rates.add(time.time(), {x["sid"]: {"rate": x.get("rate"), "late": x.get("late_max_ms"),
                                                "margin": x.get("video_margin_min_ms"), "cc": cc.get(prim.get(x["sid"]))}
                                     for c in st.get("carriers", []) for x in c.get("services", [])})
        self.stats_exporter.send(dict(st, alerts=list(self.alerts.active.values())))

    def engine_poll(self):
        if self.engine is None:
            if time.monotonic() >= self.engine_next:
                self.start_engine()
            return
        rc = self.engine.poll()
        if rc is not None:
            self.engine_restarts += 1
            wait = min(30, 2 ** min(self.engine_restarts, 5))
            log.error("engine exited (%s), restart in %d s", rc, wait)
            self.engine = None
            self.engine_next = time.monotonic() + wait

    def engine_cmd(self, cmd):
        if self.engine and self.engine.poll() is None:
            with self.cmd_lock:                     # the web threads (previews) send commands too
                try:
                    self.engine.stdin.write((cmd + "\n").encode())
                    self.engine.stdin.flush()
                except OSError:
                    pass

    def restart_stalled(self):
        """an ffmpeg still running but sending nothing (frozen HLS playlist, half-open connection): restarted,
        so that the source comes back as soon as it is available again (its service is on its backup meanwhile)"""
        lim = float(self.cfg["ingest"].get("stall_restart_s", 0) or 0)
        if lim <= 0 or not self.stats or time.time() - self.stats_at > 5:
            return
        idle = {i["index"]: i.get("idle_ms", -1) for i in self.stats.get("inputs", [])}
        now = time.monotonic()
        for p in self.ingest:
            if p.state != "running" or p.proc is None or now - p.started_at < lim or p.inp.index not in idle:
                continue
            ms = idle[p.inp.index]
            if ms < 0 or ms > lim * 1000:
                log.warning("input %s: nothing received for %.0f s, restarting it", p.inp.name,
                            (now - p.started_at) if ms < 0 else ms / 1000)
                p.restarts += 1
                p.restart()

    def restart_discontinuous(self):
        """an ffmpeg that sees a timestamp discontinuity again and again (seen after a reconnection of an
        IPTV stream: one every 0.3 s for an hour, each one making the picture jump 170 ms): it does not
        recover by itself, so its source is started again beside it and swapped in without a cut"""
        n = int(self.cfg["ingest"].get("discontinuity_restart", 10) or 0)
        if n <= 0:
            return
        now = time.monotonic()
        for p in list(self.ingest):
            if p.state != "running" or now - p.started_at < 30:
                continue
            recent = sum(1 for t in p.disc_times if now - t <= 30)
            if recent < n:
                continue
            p.disc_times.clear()
            svc = p.inp.service
            log.warning("input %s: %d timestamp discontinuities in 30 s, starting its source again", p.inp.name, recent)
            if p.inp.role == "primary" and svc is not None and svc.input is p.inp:
                self._fo_drop(svc.sid, forget=False)
                # a source received on a local port (udp://, rtp://, srt:// in listen mode, rtmp ?listen) can not
                # be opened twice: the old one is stopped first (a short cut)
                listen = bool(re.match(r"(udp|rtp|srt)://", svc.source)) or "listen" in svc.source
                self._use_source(svc, svc.active, old_down=listen)
            else:
                p.restarts += 1
                p.restart()

    # ---------------------------------------------------------------- alerts

    def check_alerts(self):
        a = self.cfg["alerts"]
        conds = {}
        st = self.stats
        if self.engine is None or (st and time.time() - self.stats_at > 10):
            conds["engine"] = ("critical", "engine not running")
        if st:
            ins = {i["index"]: i for i in st.get("inputs", [])}
            for c in st.get("carriers", []):
                car = self.plan.carriers[c["carrier"]]
                occ = (c["rate"] - c["null_rate"]) / c["ts_rate"] * 100 if c["ts_rate"] else 0
                demand = c["si_rate"]
                for s in car.services:
                    if s.input and s.input.index in ins:
                        demand += ins[s.input.index].get("rate", 0)
                if demand > c["ts_rate"]:
                    conds[f"overload.{car.index}"] = ("critical", f"{car.name}: input demand {demand / 1e6:.2f} Mbit/s "
                                                      f"exceeds the channel capacity {c['ts_rate'] / 1e6:.2f} Mbit/s")
                elif occ >= float(a["overload_percent"]):
                    conds[f"full.{car.index}"] = ("warning", f"{car.name}: multiplex {occ:.1f}% full")
                by_sid = {x.sid: x for x in car.services}
                for s in c.get("services", []):
                    svc = by_sid.get(s["sid"])
                    if svc is None:              # just removed
                        continue
                    vm = s.get("video_margin_min_ms")
                    if vm is not None and vm < 0:
                        conds[f"margin.{svc.sid}"] = ("critical", f"{svc.name}: frames sent {-vm:.0f} ms after their decoding time")
                    elif vm is not None and vm < float(a["margin_min_ms"]):
                        conds[f"margin.{svc.sid}"] = ("warning", f"{svc.name}: decoding margin down to {vm:.0f} ms")
                    if s.get("dropped_late"):
                        conds[f"drop.{svc.sid}"] = ("critical", f"{svc.name}: {s['dropped_late']} late packets dropped")
                    elif (s.get("late_max_ms") or 0) > float(a["late_ms"]):
                        conds[f"late.{svc.sid}"] = ("warning", f"{svc.name}: packets up to {s['late_max_ms']:.0f} ms late")
                    if s.get("source") == "backup":
                        what = "its sources are down" if len(svc.sources or ()) > 1 else "primary source down"
                        conds[f"backup.{svc.sid}"] = ("warning", f"{svc.name}: on backup, {what}")
                    elif svc.active:
                        conds[f"fallback.{svc.sid}"] = ("warning", f"{svc.name}: on its source {svc.active + 1} of "
                                                        f"{len(svc.sources)} ({svc.source}), the preferred ones are down")
                    elif s.get("source") == "none":
                        conds[f"nosrc.{svc.sid}"] = ("critical", f"{svc.name}: no source")
            for i in st.get("inputs", []):
                if i.get("faster_than_realtime"):
                    conds[f"fast.{i['index']}"] = ("warning", f"input {i['name']}: arrives faster than real time (readrate?)")
            for r in st.get("radios", []):
                k = f"underrun.{r['index']}"
                prev = getattr(self, "_underruns", {}).get(r["index"], r["underruns"])
                if r["underruns"] > prev:
                    conds[k] = ("critical", f"radio {r['index']}: {r['underruns'] - prev} underruns (CPU or USB too slow)")
                self.__dict__.setdefault("_underruns", {})[r["index"]] = r["underruns"]
                # gaps the radio itself reports (HackRF: its FIFO ran dry, USB transfers too late)
                prev = getattr(self, "_late", {}).get(r["index"], r.get("late", 0))
                if r.get("late", 0) > prev:
                    conds[f"late.radio{r['index']}"] = ("critical", f"radio {r['index']}: {r['late'] - prev} gaps on air "
                                                         "reported by the radio (USB path too slow: hub, dock?)")
                self.__dict__.setdefault("_late", {})[r["index"]] = r.get("late", 0)
        for p in self.ingest:
            if p.state in ("backoff", "error") and p.restarts >= 2:
                conds[f"ingest.{p.inp.index}"] = ("warning", f"input {p.inp.name}: {p.state}, {p.restarts} restarts "
                                                  f"({p.error or (p.lines[-1] if p.lines else '')})"[:300])
        for an in self.analyzers:
            s = an.summary()
            if s and (s.get("transport_errors") or 0) > 0:
                conds[f"tsduck.{an.car.index}"] = ("warning", f"{an.car.name}: tsduck sees {s['transport_errors']} transport errors")
        self.alerts.evaluate(conds)

    # ---------------------------------------------------------------- main loop

    def run(self):
        def on_stop(*_):
            self._stop = True

        def on_hup(*_):
            self._reload = True
        signal.signal(signal.SIGTERM, on_stop)
        signal.signal(signal.SIGINT, on_stop)
        signal.signal(signal.SIGHUP, on_hup)
        self.si.tick(force=True)
        self.start_engine()
        gpu.MANAGER.start()             # detection only; the learning, when needed, in the background
        for p in self.ingest:
            p.start()
        web = None
        if self.cfg["web"].get("enabled", True) and self.cfg["web"].get("listen"):
            from . import web as webmod
            try:
                web = webmod.start(self, self.cfg["web"]["listen"])
            except OSError as e:
                log.error("web page on %s: %s (running without it)", self.cfg["web"]["listen"], e)
        tick = 0
        while not self._stop:
            self._run_calls(1.0)
            if self._stop:
                break
            tick += 1
            if self._reload:
                self._reload = False
                self.reload_si()
                self.channels_reload()
            self.engine_poll()
            for p in self.ingest:
                p.poll()
            self.restart_stalled()
            self.restart_discontinuous()
            self._failover()
            self._retire()
            for an in self.analyzers:
                an.poll()
            self.previews.poll()
            try:
                self.si.tick()
            except Exception as e:                          # noqa: BLE001
                log.exception("SI update failed: %s", e)
            if tick % 2 == 0:
                self.check_alerts()
        log.info("stopping")
        for p in self.ingest + [r[0] for r in self.retiring] + [f["cand"]["proc"] for f in self.fo.values() if f.get("cand")]:
            p.stop()
        for an in self.analyzers:
            an.stop()
        self.previews.stop()
        if self.engine and self.engine.poll() is None:
            self.engine_cmd("quit")
            try:
                self.engine.wait(5)
            except subprocess.TimeoutExpired:
                self.engine.kill()
        if web:
            web.shutdown()

    def reload_si(self):
        log.info("reloading EPG and SI tables")
        self.si.next_sched = 0
        self.si.tick(force=True)

    # ---------------------------------------------------------------- channels while running

    def call(self, fn, timeout=120):
        """runs fn() in the main loop (from a web thread) and returns its result"""
        c = _Call(fn)
        self.calls.put(c)
        if not c.ev.wait(timeout):
            raise chmod.ChannelError("the transmitter did not answer in time")
        if c.error:
            raise c.error
        return c.result

    def _run_calls(self, seconds):
        end = time.monotonic() + seconds
        while not self._stop:
            left = end - time.monotonic()
            if left <= 0:
                return
            try:
                c = self.calls.get(timeout=left)
            except queue.Empty:
                return
            try:
                c.result = c.fn()
            except chmod.ChannelError as e:
                c.error = e
            except (cfgmod.ConfigError, OSError) as e:      # an invalid file, a file that cannot be written
                c.error = chmod.ChannelError(str(e))
            except Exception as e:                       # noqa: BLE001 - reported to the caller
                log.exception("channel change failed")
                c.error = chmod.ChannelError(f"internal error: {e}")
            c.ev.set()

    def engine_request(self, text, timeout=5):
        """command to the engine and its answer; when the engine is not running, nothing to do: it starts
        again from the plan (engine.ini), which already has the change"""
        if not (self.engine and self.engine.poll() is None):
            return {"ok": True, "note": "engine not running"}
        with self.reply_cv:
            self.seq += 1
            seq = self.seq
        self.engine_cmd(f"seq={seq} {text}")
        with self.reply_cv:
            self.reply_cv.wait_for(lambda: seq in self.replies, timeout)
            return self.replies.pop(seq, {"ok": False, "error": "no answer from the engine"})

    def proc_of(self, index):
        return next((p for p in self.ingest if p.inp.index == index), None)

    def _new_input_index(self):
        """a fresh input id. Every source change takes one: they are taken in turn and, past the last one,
        from 0 again, skipping the ids still in use (the UDP port is base_port + id: the ids stay below
        the engine's 1024 and the monitor ports)"""
        ing = self.cfg["ingest"]
        gap = int(ing["monitor_base_port"]) - int(ing["base_port"])
        top = min(1024, gap) if gap > 0 else 1024
        used = {i.index for i in self.plan.inputs} | {p.inp.index for p in self.ingest} \
            | {r[1] for r in self.retiring} | {f["cand"]["inp"].index for f in self.fo.values() if f.get("cand")} \
            | {i["index"] for i in (self.stats or {}).get("inputs", [])}
        for _ in range(top):
            i = self.next_input if self.next_input < top else 0
            self.next_input = i + 1
            if i not in used:
                return i
        raise RuntimeError(f"no input id left below {top}")

    def channel_add(self, spec, allow_exec=False):
        new_cfg, new_plan, sid = chmod.add(self.cfg, self.plan, spec, allow_exec)
        return dict(self._apply(new_cfg, new_plan), sid=sid)

    def channel_remove(self, ref):
        s = chmod.find(self.plan, ref)
        new_cfg, new_plan, sid = chmod.remove(self.cfg, self.plan, ref)
        return dict(self._apply(new_cfg, new_plan, removed=(s.carrier.index, s.local_index)), sid=sid)

    def channel_update(self, ref, changes, allow_exec=False):
        new_cfg, new_plan, sid = chmod.update(self.cfg, self.plan, ref, changes, allow_exec)
        return dict(self._apply(new_cfg, new_plan), sid=sid)

    # ---------------------------------------------------------------- the configuration file edited from the web
    # page: every change is saved in the file at once, the transmitter follows when asked (apply_config)

    def draft(self):
        if not self.plan.config_path:
            raise chmod.ChannelError("no configuration file to edit (channels set on the command line)")
        return confedit.Draft(self.plan.config_path, self._validate_text)

    def _validate_text(self, text):
        from .cli import load_cfg
        cfg = load_cfg(self.args, text=text) if self.args is not None else \
            cfgmod.load(self.plan.config_path, text=text)
        return cfg, cfgmod.build(cfg)

    def config_view(self):
        """the file (radios, multiplexes), whether it is valid, what differs from what runs, the profiles"""
        if not self.plan.config_path:
            return {"path": None, "error": "no configuration file (channels set on the command line)"}
        d = self.draft()
        out = {"path": self.plan.config_path, "error": None, "drivers": sorted(cfgmod.DRIVER_RATES),
               "std_keys": cfgmod.STD_KEYS, "radio_defaults": cfgmod.RADIO_DEFAULTS}
        try:
            text = d.text()
            doc = tomllib.loads(text)
        except (OSError, tomllib.TOMLDecodeError) as e:
            return dict(out, error=str(e))
        try:
            _, plan = self._validate_text(text)
            caps = {c.index: c.ts_rate for c in plan.carriers}
        except cfgmod.ConfigError as e:
            out["error"], caps = str(e), {}
        out["network"] = dict(cfgmod.DEFAULTS["network"], **doc.get("network", {}))
        out["network_keys"] = list(confedit.NETWORK_KEYS)
        out["radios"] = doc.get("radio", [])
        out["carriers"] = [dict({k: v for k, v in c.items() if k != "service"}, services=len(c.get("service", [])),
                                capacity=caps.get(i)) for i, c in enumerate(doc.get("carrier", []))]
        out["pending"] = confedit.diff(self.applied_doc, doc)
        out["restart"] = confedit.needs_restart(self.applied_doc, doc)
        out["profiles"] = d.profiles()
        return out

    def draft_channel(self, op, ref=None, spec=None, allow_exec=False):
        """a channel added, changed or removed in the file only (pending until applied)"""
        res = {}

        def fn(cfg, plan):
            removed = None
            if op == "add":
                new, _, sid = chmod.add(cfg, plan, spec, allow_exec)
            elif op == "remove":
                svc = chmod.find(plan, ref)
                removed = (svc.carrier.index, svc.local_index)
                new, _, sid = chmod.remove(cfg, plan, ref)
            else:
                new, _, sid = chmod.update(cfg, plan, ref, spec, allow_exec)
            res["sid"] = sid
            return new, removed
        return dict(self.draft().channels(fn), pending=True, **res)

    def apply_config(self):
        """the file applied: the channels one by one when only they changed, else the transmission restarted"""
        text = self.draft().text()
        try:
            new_cfg, new_plan = self._validate_text(text)
        except cfgmod.ConfigError as e:
            raise chmod.ChannelError(str(e)) from e
        new_plan.config_path = self.plan.config_path
        doc = tomllib.loads(text)
        changes = confedit.diff(self.applied_doc, doc)
        if confedit.needs_restart(self.applied_doc, doc):
            self.restart_plan(new_cfg, new_plan)
            res = {"restarted": True}
        else:
            res = dict(self._apply(new_cfg, new_plan, save=False), restarted=False)
        self.applied_doc = doc
        log.info("configuration applied: %d change(s)%s", len(changes), ", transmission restarted" if res["restarted"] else "")
        return dict(res, detail=changes)

    def revert_config(self):
        """pending changes dropped: the file back to what runs (its text as last applied is not kept: the
        running radios, multiplexes and channels are written over the file's)"""
        d = self.draft()
        cur = d.doc()
        if not confedit.diff(self.applied_doc, cur):
            return {"saved": False}
        t = confedit.Tables(d.path)
        want = self.applied_doc
        lines_ok = len(cur.get("radio", [])) == len(want.get("radio", [])) and \
            len(cur.get("carrier", [])) == len(want.get("carrier", []))
        if not lines_ok:
            raise chmod.ChannelError("radios or multiplexes were added or removed: load a profile, or edit them back")
        for sec in sorted(set(cur) | set(want)):            # [network] and the other plain tables
            a, b = cur.get(sec, {}), want.get(sec, {})
            if sec in ("radio", "carrier") or a == b or not isinstance(a, dict) or not isinstance(b, dict):
                continue
            if any(isinstance(v, dict) for v in list(a.values()) + list(b.values())):
                raise chmod.ChannelError(f"[{sec}] changed in sub-tables: load a profile, or edit it back")
            t.set_section_keys(sec, {k: v for k, v in b.items() if a.get(k) != v}, [k for k in a if k not in b])
        for kind in ("radio", "carrier"):
            for i, (a, b) in enumerate(zip(cur.get(kind, []), want.get(kind, []))):
                a2 = {k: v for k, v in a.items() if k != "service"}
                b2 = {k: v for k, v in b.items() if k != "service"}
                if a2 != b2:
                    t.set_table_keys(kind, i, {k: v for k, v in b2.items() if a2.get(k) != v},
                                     [k for k in a2 if k not in b2])
        t.save()
        old_cfg, _ = self._validate_text(d.text())
        new_cfg = copy.deepcopy(old_cfg)
        for ci, c in enumerate(want.get("carrier", [])):
            new_cfg["carrier"][ci]["service"] = copy.deepcopy(c.get("service", []))
        if [c.get("service", []) for c in old_cfg["carrier"]] != [c.get("service", []) for c in new_cfg["carrier"]]:
            chmod.persist(d.path, old_cfg, new_cfg)
        return {"saved": True}

    def restart_plan(self, new_cfg, new_plan):
        """radios or multiplexes changed: the engine started again on the new plan (a few seconds without
        signal); the sources that stay the same keep running"""
        log.warning("configuration: radios or multiplexes changed, the transmission restarts")
        self.previews.stop()
        for an in self.analyzers:
            an.stop()
        for sid in list(self.fo):
            self._fo_drop(sid)
        for r in self.retiring:
            r[0].stop()
        self.retiring = []
        if self.engine and self.engine.poll() is None:
            self.engine_cmd("quit")
            try:
                self.engine.wait(8)
            except subprocess.TimeoutExpired:
                self.engine.kill()
                self.engine.wait()
        self.engine = None

        def key(inp):
            return (inp.addr, inp.source, inp.kind, inp.role, json.dumps(inp.options, sort_keys=True, default=str))
        old = {key(p.inp): p for p in self.ingest}
        procs = []
        for inp in new_plan.inputs:
            p = old.pop(key(inp), None)
            if p is None:
                p = IngestProc(inp, new_plan)
            else:
                p.inp, p.plan, p.ing = inp, new_plan, new_cfg["ingest"]
            procs.append(p)
        for p in old.values():
            p.stop()
        self.ingest = procs
        self.plan, self.cfg = new_plan, new_cfg
        new_plan.cfg = new_cfg
        if self.store:
            self.store.services = new_plan.services
            try:
                self.store.refresh(remap=True)
            except Exception as e:                          # noqa: BLE001
                log.warning("EPG: %s", e)
        self.si = SiManager(new_plan, self.si_dir, self.store, self.detected)
        self.analyzers = self._make_analyzers()
        self.next_input = max((i.index for i in new_plan.inputs), default=-1) + 1
        self.fo, self.fo_wait = {}, {}
        self.si.tick(force=True)
        self.start_engine()
        self.engine_restarts = 0
        for p in self.ingest:
            if p.proc is None:
                p.start()

    def channels(self):
        live = {}
        for c in (self.stats or {}).get("carriers", []):
            for x in c.get("services", []):
                live[x["sid"]] = x.get("source")
        return [dict(chmod.describe(s), on=live.get(s.sid)) for s in self.plan.services]

    def channels_reload(self):
        """SIGHUP: the channels of the configuration file applied (the other settings need a restart)"""
        if not self.plan.config_path or self.args is None:
            return
        try:
            from .cli import load_cfg
            new_cfg = load_cfg(self.args)
            new_plan = cfgmod.build(new_cfg)
        except (cfgmod.ConfigError, OSError) as e:
            log.error("configuration not reloaded: %s", e)
            return

        def rest(c):
            d = copy.deepcopy(c)
            for car in d.get("carrier", []):
                car.pop("service", None)
            d.pop("defaults", None)
            return d
        old_r, new_r = rest(self.cfg), rest(new_cfg)
        if old_r.get("carrier") != new_r.get("carrier") or old_r.get("radio") != new_r.get("radio"):
            log.error("configuration reload: the radios or multiplexes changed: restart dvbtx to apply; "
                      "channels left as they are")
            return
        if old_r != new_r:
            log.warning("configuration reload: settings other than the channels changed: restart dvbtx to apply them")
        try:
            r = self._apply(new_cfg, new_plan, save=False)
            log.info("configuration reload: %s", "; ".join(r["changes"]) or "no channel change")
        except chmod.ChannelError as e:
            log.error("configuration reload: %s", e)

    def _apply(self, new_cfg, new_plan, removed=None, save=True):
        """Makes the running transmitter match new_plan (same radios and multiplexes, other channels):
        only the channels that changed are touched. Returns {"changes": [...], "saved": bool, ...}."""
        plan = self.plan
        old = {s.sid: s for s in plan.services}
        new = {s.sid: s for s in new_plan.services}
        base = int(self.cfg["ingest"]["base_port"])
        skip = chmod.SI_KEYS | chmod.ENGINE_KEYS | {"source", "backup"}

        def eng_key(s):
            return (s.carrier.index, s.pid_base, tuple(s.opts.get(k) for k in sorted(chmod.ENGINE_KEYS - {"sid", "pid_base"})))

        skip |= chmod.SUPERVISOR_KEYS

        def ing_key(s):
            return (tuple(s.sources or [s.source]),
                    json.dumps({k: v for k, v in s.opts.items() if k not in skip}, sort_keys=True, default=str))

        gone = [s for sid, s in old.items() if sid not in new or eng_key(s) != eng_key(new[sid])]
        came = [n for sid, n in new.items() if sid not in old or eng_key(old[sid]) != eng_key(n)]
        kept = [(old[sid], n) for sid, n in new.items() if sid in old and eng_key(old[sid]) == eng_key(n)]
        backups = {i.source: i for i in plan.inputs if i.role == "backup"}
        added_inputs, swaps, backup_set, renamed, changes, detail = [], [], [], [], [], []

        def change(what, name, before=None, after=None, text=""):
            changes.append(text)
            detail.append({"what": what, "name": name, "from": before, "to": after})

        def backup_of(n):
            if not n.backup:
                return None
            if n.backup not in backups:
                backups[n.backup] = cfgmod.backup_input(self._new_input_index(), n, base)
                added_inputs.append(backups[n.backup])
            return backups[n.backup]

        def primary_of(n):
            inp = cfgmod.primary_input(self._new_input_index(), n, base)
            if inp.kind == "direct" and any(i.kind == "direct" and i.addr == inp.addr for i in plan.inputs):
                raise chmod.ChannelError(f"{inp.addr} is already received for another channel")
            added_inputs.append(inp)
            return inp

        for n in came:
            primary_of(n)
            n.backup_input = backup_of(n)
            change("added" if n.sid not in old else "recreated", n.name,
                   text=f"{n.name} added" if n.sid not in old else f"{n.name} re-created")
        for o, n in kept:
            if ing_key(o) != ing_key(n):
                if o.input.kind == "direct" and n.source == o.source:
                    n.input = o.input                      # same address: options apply at the next start
                else:
                    primary_of(n)
                    swaps.append((o, n))
                    nl = n.sources or [n.source]
                    change("source", n.name, o.source, nl[0] if len(nl) == 1 else nl,
                           f"{n.name}: new source {', then '.join(nl)}")
            else:
                n.input, n.source, n.active = o.input, o.source, o.active     # the source in use stays
            n.input.service, n.input.name = n, n.name
            n.input.options = cfgmod.input_options(n.input.source, n.opts)
            if n.backup != o.backup:
                n.backup_input = backup_of(n)
                backup_set.append(n)
                change("backup", n.name, o.backup, n.backup, f"{n.name}: backup {n.backup or 'none'}")
            else:
                n.backup_input = o.backup_input
            if n.name != o.name:
                renamed.append(n)
                change("name", n.name, o.name, n.name, f"{o.name} renamed {n.name}")
            if n.lcn != o.lcn:
                change("number", n.name, o.lcn, n.lcn, f"{n.name}: number {o.lcn} -> {n.lcn}")
            if (n.provider, n.visible, n.type, n.epg_id) != (o.provider, o.visible, o.type, o.epg_id):
                change("si", n.name, text=f"{n.name}: service information")
        for s in gone:
            if s.sid not in new:
                change("removed", s.name, text=f"{s.name} removed")

        # sources being tried for the channels whose source changes (or that go): abandoned
        for sid in {s.sid for s in gone} | {n.sid for n in came} | {n.sid for _, n in swaps}:
            self._fo_drop(sid)

        # 1. new inputs first: if the engine refuses one, nothing has changed yet
        running = self.engine is not None and self.engine.poll() is None
        done = []
        for inp in added_inputs:
            o = inp.options
            r = self.engine_request(f"input add id={inp.index} addr={inp.addr} program={int(o.get('program', 0))} "
                                    f"buffer_ms={int(o['buffer_ms'])} ring_pkts={int(o['ring_pkts'])} "
                                    f"name={inp.name[:60]}")
            if not r["ok"]:
                for d in done:
                    self.engine_request(f"input del id={d.index}")
                raise chmod.ChannelError(f"input {inp.name}: {r.get('error')}")
            done.append(inp)

        # 2. the engine's services
        bad = []

        def eng(cmd):
            r = self.engine_request(cmd)
            if not r["ok"]:
                bad.append(f"{cmd.split(' name=')[0]}: {r.get('error')}")

        for s in gone:
            eng(f"service del carrier={s.carrier.index} sid={s.sid}")
        for n in came:
            o = n.opts
            eng(f"service add carrier={n.carrier.index} sid={n.sid} pid_base={n.pid_base} primary={n.input.index} "
                f"backup={n.backup_input.index if n.backup_input else -1} "
                f"switch_timeout_ms={int(o['switch_timeout_ms'])} return_ms={int(o['return_ms'])} "
                f"max_late_ms={int(o['max_late_ms'])} early_ms={int(o['early_ms'])} pcr_max_ms={int(o['pcr_max_ms'])} "
                f"pmt_period_ms={int(o['pmt_period_ms'])} priority={int(o.get('priority', 5))} name={n.name}")
        for o, n in swaps:
            eng(f"service set carrier={n.carrier.index} sid={n.sid} primary={n.input.index}")
        for n in backup_set:
            eng(f"service set carrier={n.carrier.index} sid={n.sid} backup={n.backup_input.index if n.backup_input else -1}")
        for n in renamed:
            eng(f"service set carrier={n.carrier.index} sid={n.sid} name={n.name}")

        # 3. inputs nobody uses any more: the engine frees them once released (a replaced source plays
        # until its successor delivers: its ffmpeg is stopped when the engine has let it go)
        used = {n.input.index for n in new_plan.services} | {n.backup_input.index for n in new_plan.services
                                                            if n.backup_input}
        swapped = {o.input.index for o, _ in swaps}
        for inp in plan.inputs:
            if inp.index in used:
                continue
            eng(f"input del id={inp.index}")
            p = self.proc_of(inp.index)
            if p:
                self.ingest.remove(p)
                if inp.index in swapped:
                    self.retiring.append([p, inp.index, time.monotonic() + 30])
                else:
                    p.stop()
        for inp in added_inputs:
            self.ingest.append(IngestProc(inp, plan))         # started by the main loop

        # 4. the plan now describes the new channels (the same radio and multiplex objects)
        for car in plan.carriers:
            car.services = [n for n in new_plan.services if n.carrier.index == car.index]
            for n in car.services:
                n.carrier = car
        plan.services = list(new_plan.services)
        plan.inputs = sorted([i for i in plan.inputs if i.index in used] + added_inputs, key=lambda i: i.index)
        old_cfg = self.cfg
        plan.cfg = self.cfg = new_cfg
        if self.store:
            self.store.services = plan.services
            try:
                self.store.refresh(remap=True)
            except Exception as e:                          # noqa: BLE001 - a broken EPG must not stop the TV
                log.warning("EPG: %s", e)
        now = dt.datetime.now(UTC)
        self.si.write_static()
        self.si.write_pf(now)
        if self.cfg["epg"].get("schedule", True):
            self.si.write_schedule(now)
        for c in changes:
            log.info("channels: %s", c)
        if bad:
            # the engine and the plan disagree: restarted from the plan (every channel cut a few seconds)
            log.error("engine refused a change (%s): restarting it", "; ".join(bad))
            if self.engine and self.engine.poll() is None:
                self.engine.kill()

        # 5. the configuration file
        res = {"changes": changes, "detail": detail, "saved": False, "applied": running}
        if not save:
            return res
        if not plan.config_path:
            res["warning"] = "not saved: no configuration file, or channels set on the command line"
            return res
        try:
            chmod.persist(plan.config_path, old_cfg, new_cfg, removed)
            res["saved"] = True
            doc = self._file_doc()
            for ci, c in enumerate(self.applied_doc.get("carrier", [])):
                if ci < len(doc.get("carrier", [])):
                    c["service"] = copy.deepcopy(doc["carrier"][ci].get("service", []))
        except (chmod.ChannelError, OSError) as e:
            res["warning"] = f"not saved in {plan.config_path}: {e}"
            log.warning("channels: %s", res["warning"])
        return res

    # ---------------------------------------------------------------- several sources per channel

    def _failover(self):
        """Channels with several sources (source = [first choice, second, ...]): when the source in use
        has delivered nothing for failover_s (or has not started within source_start_s), the next one
        is used (after the last, the first again), the channel playing its backup meanwhile; while on
        another than the first, the preferred ones are tried every preferred_retry_s in order, beside
        the source in use, and the channel goes back to one as soon as it delivers (without a cut).
        A source is also left when it is unstable: the channel fell back to its backup unstable_switches
        times within unstable_window_s, or stayed on it failover_s + return_ms. Each time a source is
        left, it is tried again twice later than the previous time (up to an hour)."""
        if not self.stats or time.time() - self.stats_at > 5 or not (self.engine and self.engine.poll() is None):
            return
        ins = {i["index"]: i for i in self.stats.get("inputs", [])}
        live = {x["sid"]: x.get("source") for c in self.stats.get("carriers", []) for x in c.get("services", [])}
        now = time.monotonic()
        for svc in list(self.plan.services):
            n = len(svc.sources or ())
            if n < 2 or svc.input is None:
                continue
            o = svc.opts
            fail_ms = float(o.get("failover_s", 10)) * 1000
            start_s = float(o.get("source_start_s", 30))
            retry_s = float(o.get("preferred_retry_s", 300))
            st = self.fo.setdefault(svc.sid, {"since": now, "next_probe": now + retry_s, "probe_k": 0})
            i = ins.get(svc.input.index)
            idle = i.get("idle_ms", -1) if i else -1
            why = None
            if i and i.get("streams") and 0 <= idle:
                if idle > fail_ms:
                    why = f"nothing for {idle / 1000:.0f} s"
            elif now - st["since"] > start_s:                # never delivered: given source_start_s
                why = f"not started within {start_s:.0f} s"
            # unstable: back and forth to the backup
            on = live.get(svc.sid)
            drops = st.setdefault("drops", [])
            if st.get("on") == "primary" and on == "backup":
                drops.append(now)
            st["on"] = on
            win = float(o.get("unstable_window_s", 600))
            drops[:] = [t for t in drops if now - t <= win]
            if on == "backup":
                st.setdefault("bk_since", now)
            else:
                st.pop("bk_since", None)
            stay = fail_ms / 1000 + float(o.get("return_ms", 10000)) / 1000
            if why is None and len(drops) >= int(o.get("unstable_switches", 3)) > 0:
                why = f"unstable: {len(drops)} falls to the backup within {win / 60:.0f} min"
            elif why is None and st.get("bk_since") and now - st["bk_since"] >= stay and now - st["since"] > start_s:
                why = f"on the backup for {now - st['bk_since']:.0f} s"
            if now - st["since"] > 1800:                     # stable for 30 min: forgiven
                self.fo_wait.get(svc.sid, {}).pop(svc.active, None)
            if why:
                k = (svc.active + 1) % n
                w = self.fo_wait.setdefault(svc.sid, {}).get(svc.active, [retry_s / 2, 0])
                w[0] = min(max(w[0] * 2, retry_s), 3600)
                w[1] = now + w[0]
                self.fo_wait[svc.sid][svc.active] = w
                log.warning("%s: source %d of %d left (%s: %s), now on source %d: %s", svc.name, svc.active + 1, n,
                            svc.source, why, k + 1, svc.sources[k])
                self._fo_drop(svc.sid)
                self._use_source(svc, k, old_down=True)
                continue
            # back to a preferred source
            c = st.get("cand")
            if c:
                c["proc"].poll()
                ci = ins.get(c["inp"].index)
                ok = ci and ci.get("streams") and 0 <= ci.get("idle_ms", -1) < 1000
                if not ok:
                    c["ok_since"] = None
                elif c["ok_since"] is None:
                    c["ok_since"] = now
                if ok and now - c["ok_since"] >= 2:   # short: some IPTV accounts tolerate a second connection briefly
                    log.info("%s: source %d (%s) is back, using it again", svc.name, c["k"] + 1, svc.sources[c["k"]])
                    st["cand"] = None
                    self._use_source(svc, c["k"], c["inp"], c["proc"])
                elif (c["ok_since"] is None and now - c["started"] > start_s) or now - c["started"] > start_s + 15:
                    self._fo_drop(svc.sid, forget=False)
                    st["probe_k"] = c["k"] + 1
                    if st["probe_k"] >= svc.active:
                        st["probe_k"], st["next_probe"] = 0, now + retry_s
                continue
            if svc.active and retry_s > 0 and now >= st["next_probe"]:
                waits = self.fo_wait.get(svc.sid, {})
                due = [j for j in range(st["probe_k"], svc.active) if now >= waits.get(j, [0, 0])[1]]
                if due:
                    self._fo_try(svc, due[0])
                else:
                    st["probe_k"], st["next_probe"] = 0, now + retry_s

    def _fo_try(self, svc, k):
        """starts source k of svc beside the one in use (an input the engine receives, used by no service)"""
        inp = cfgmod.primary_input(self._new_input_index(), svc, int(self.cfg["ingest"]["base_port"]), src=svc.sources[k])
        if not self._engine_input_add(inp):
            self.fo[svc.sid]["next_probe"] = time.monotonic() + float(svc.opts.get("preferred_retry_s", 300))
            return
        proc = IngestProc(inp, self.plan)
        proc.poll()
        self.fo[svc.sid]["cand"] = {"k": k, "inp": inp, "proc": proc, "started": time.monotonic(), "ok_since": None}
        log.info("%s: trying its source %d again (%s)", svc.name, k + 1, svc.sources[k])

    def _fo_drop(self, sid, forget=True):
        """abandons the source being tried for a channel (forget: and the state of the channel)"""
        st = self.fo.pop(sid, None) if forget else self.fo.get(sid)
        c = st and st.get("cand")
        if c:
            st["cand"] = None
            self.engine_request(f"input del id={c['inp'].index}")
            c["proc"].stop()

    def _engine_input_add(self, inp):
        o = inp.options
        r = self.engine_request(f"input add id={inp.index} addr={inp.addr} program={int(o.get('program', 0))} "
                                f"buffer_ms={int(o['buffer_ms'])} ring_pkts={int(o['ring_pkts'])} name={inp.name[:60]}")
        if not r["ok"]:
            log.error("input %s: %s", inp.name, r.get("error"))
        return r["ok"]

    def _use_source(self, svc, k, inp=None, proc=None, old_down=False):
        """svc plays its source k from now on: started if not yet (inp/proc: already running), swapped in
        without a cut, the previous one stopped once the engine has let it go (at once if it is down:
        it may hold what the next one needs, a UDP port, the only connection an IPTV account allows)"""
        if inp is None:
            inp = cfgmod.primary_input(self._new_input_index(), svc, int(self.cfg["ingest"]["base_port"]),
                                       src=(svc.sources or [svc.source])[k])
            if not self._engine_input_add(inp):
                self.fo[svc.sid] = {"since": time.monotonic(), "next_probe": 0, "probe_k": 0}
                return
            proc = IngestProc(inp, self.plan)
        old = svc.input
        r = self.engine_request(f"service set carrier={svc.carrier.index} sid={svc.sid} primary={inp.index}")
        if not r["ok"]:
            log.error("%s: source %d refused by the engine: %s", svc.name, k + 1, r.get("error"))
            self.engine_request(f"input del id={inp.index}")
            proc.stop()
            return
        self.engine_request(f"input del id={old.index}")
        p = self.proc_of(old.index)
        if p:
            self.ingest.remove(p)
            if old_down:
                p.stop()
            else:
                self.retiring.append([p, old.index, time.monotonic() + 30])
        self.ingest.append(proc)
        svc.input, svc.source, svc.active = inp, (svc.sources or [svc.source])[k], k
        inp.service = svc
        self.plan.inputs = sorted([i for i in self.plan.inputs if i.index != old.index] + [inp], key=lambda i: i.index)
        now = time.monotonic()
        self.fo[svc.sid] = {"since": now, "next_probe": now + float(svc.opts.get("preferred_retry_s", 300)), "probe_k": 0}

    def _retire(self):
        """old sources of channels whose source changed: stopped once the engine has released them"""
        if not self.retiring:
            return
        live = {i["index"] for i in (self.stats or {}).get("inputs", [])} if time.time() - self.stats_at < 5 else None
        now = time.monotonic()
        for r in list(self.retiring):
            p, idx, deadline = r
            if (live is not None and idx not in live) or now > deadline:
                p.stop()
                self.retiring.remove(r)

    def _transcoded(self, s):
        """the channel is re-encoded: what its ffmpeg does, else what its settings ask"""
        p = self.proc_of(s.input.index) if s.input else None
        if p and p.mode:
            return p.mode.get("kind") in ("cpu", "gpu")
        return bool(s.opts.get("encode_on") or cfgmod._needs_encode(s.source))

    def status(self):
        """everything the web page shows"""
        with self.lock:
            st = self.stats
        plan = self.plan
        out = {"version": __import__("dvbtx").__version__, "uptime": round(time.time() - self.started),
               "stats_age": round(time.time() - self.stats_at, 1) if self.stats_at else None,
               "engine": {"running": self.engine is not None and self.engine.poll() is None,
                          "restarts": self.engine_restarts, "log": list(self.engine_lines)[-50:]},
               "radios": [], "carriers": [], "inputs": [p.status() for p in self.ingest] +
                                                       [dict(f["cand"]["proc"].status(), role="trial")
                                                        for f in self.fo.values() if f.get("cand")],
               "alerts": list(self.alerts.active.values()), "alert_history": list(self.alerts.history)[:50],
               "network": {k: plan.cfg["network"][k] for k in ("name", "network_id", "onid", "country", "provider")},
               "thresholds": {"margin_min_ms": self.cfg["alerts"]["margin_min_ms"]},
               "si": dict(self.si.stats), "epg_errors": self.store.errors[-5:] if self.store else [],
               "config_path": plan.config_path, "gpu": gpu.MANAGER.status() if gpu.MANAGER else None,
               "web": {"preview_thumbnails": bool(self.cfg["web"].get("preview_thumbnails", True))},
               "system": self.sysload.status(),
               "default_backup": "" if plan.cfg["defaults"].get("backup") is False else str(plan.cfg["defaults"].get("backup") or "")}
        rst = {r["index"]: r for r in (st or {}).get("radios", [])}
        for r in plan.radios:
            if r.carriers:
                out["radios"].append({"index": r.index, "name": r.name, "driver": r.driver, "center": r.center,
                                      "rate": r.rate, "gain": r.gain, "amp": r.amp, "live": rst.get(r.index)})
        cst = {c["carrier"]: c for c in (st or {}).get("carriers", [])}
        ist = {i["index"]: i for i in (st or {}).get("inputs", [])}
        for p in out["inputs"]:
            p["live"] = ist.get(p["index"])
        for c in plan.carriers:
            live = cst.get(c.index)
            svcs = []
            lsv = {x["sid"]: x for x in (live or {}).get("services", [])}
            for s in c.services:
                ls = lsv.get(s.sid)
                evs = self.store.events(s.sid) if self.store else []
                pres, fol, _ = epgmod.present_following(evs, dt.datetime.now(UTC))
                svcs.append({"name": s.name, "sid": s.sid, "lcn": s.lcn, "source": s.source, "backup": s.backup,
                             "sources": list(s.sources or [s.source]), "active": s.active,
                             "buffer_ms": s.opts.get("buffer_ms", "auto"),
                             "priority": int(s.opts.get("priority", 5)),
                             "bitrate_weight": s.opts.get("bitrate_weight", 1), "max_bitrate": s.opts.get("max_bitrate") or "",
                             "transcoded": self._transcoded(s),
                             "auto_bitrate": round(cfgmod._bitrate(s.opts["encode"].get("video_bitrate"))
                                                   if s.opts["encode"].get("video_bitrate") not in (None, "", "auto")
                                                   else cfgmod.auto_video_bitrate(s)),
                             "buffer_ms_used": s.input.options.get("buffer_ms") if s.input else None,
                             "trying": (self.fo.get(s.sid) or {}).get("cand", {}).get("k") if (self.fo.get(s.sid) or {}).get("cand") else None,
                             "type": self.si._stype(s), "live": ls,
                             "input": ist.get(s.input.index) if s.input else None,
                             "now": pres and {"title": pres.get("title"), "start": pres["start"].isoformat(),
                                              "duration": pres["duration"]},
                             "next": fol and {"title": fol.get("title"), "start": fol["start"].isoformat()},
                             "epg_events": len(evs)})
            # load of the sources of its channels: GPU per engine summed (the busiest shown), CPU summed
            eng, cpu, per = {}, 0, []
            for sv in c.services:
                p = self.proc_of(sv.input.index) if sv.input else None
                u = (p.usage if p and p.state == "running" else None) or {}
                for k, v in (u.get("gpu") or {}).items():
                    eng[k] = eng.get(k, 0) + v
                cpu += u.get("cpu") or 0
                if u.get("gpu"):
                    per.append({"name": sv.name, "gpu": max(u["gpu"].values()), "cpu": u.get("cpu")})
            an = next((a for a in self.analyzers if a.car is c), None)
            out["carriers"].append({"index": c.index, "name": c.name, "standard": c.standard, "frequency": c.frequency,
                                    "offset": c.offset, "params": c.params, "tsid": c.tsid, "capacity": c.ts_rate,
                                    "radio": c.radio.name if c.radio else None, "live": live, "services": svcs,
                                    "tsduck": an.summary() if an else None,
                                    "sources_load": {"gpu": max(eng.values()) if eng else None, "gpu_engines": eng,
                                                     "cpu": cpu, "gpu_channels": per},
                                    "monitor": f"udp://127.0.0.1:{int(plan.cfg['ingest']['monitor_base_port']) + c.index}"})
        return out
