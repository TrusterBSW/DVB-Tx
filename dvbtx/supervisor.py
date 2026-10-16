"""Supervisor: SI tables, the C engine, the ingest processes, TSDuck analysers, alerts, history."""

import collections
import datetime as dt
import hashlib
import json
import logging
import os
import shutil
import signal
import socket
import subprocess
import threading
import time
import urllib.request

from . import config as cfgmod
from . import epg as epgmod
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
        v = self.ver.get("nit", _digest(nit0))
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
        mon = self.cfg["monitoring"]
        self.exporter = Exporter(mon.get("export"))
        self.stats_exporter = Exporter(mon.get("stats_export"))
        self.analyzers = []
        use_tsp = mon.get("tsduck", "auto")
        tsp = mon.get("tsp", "tsp")
        if use_tsp is True or (use_tsp == "auto" and shutil.which(tsp)):
            base = int(self.cfg["ingest"]["monitor_base_port"])
            self.analyzers = [Analyzer(c, base + c.index, int(mon["interval"]), tsp, self.exporter) for c in plan.carriers]
        elif use_tsp is True:
            log.warning("tsduck requested but '%s' not found", tsp)
        self.alerts = Alerts(self.cfg["alerts"])
        self.history = History()
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
                k = f"s{i}.{s['index']}"
                vals[k + ".rate"] = round(s["rate"])
                vals[k + ".vmargin"] = s.get("video_margin_min_ms")
                vals[k + ".amargin"] = s.get("audio_margin_min_ms")
                vals[k + ".late"] = s.get("late_max_ms")
                # service type from the streams
                svc = self.plan.carriers[i].services[s["index"]]
                vt = s.get("video_type", -1)
                if s.get("streams"):
                    t = si.service_type_from_streams(vt if vt >= 0 else None, s.get("audio_types") or [])
                    if self.detected.get(svc.sid) != t:
                        self.detected[svc.sid] = t
        for r in st.get("radios", []):
            vals[f"r{r['index']}.cpu"] = r.get("cpu")
        self.history.add(st.get("time", time.time()), vals)
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
            try:
                self.engine.stdin.write((cmd + "\n").encode())
                self.engine.stdin.flush()
            except OSError:
                pass

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
                for s in c.get("services", []):
                    svc = car.services[s["index"]]
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
                        conds[f"backup.{svc.sid}"] = ("warning", f"{svc.name}: on backup, primary source down")
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
            time.sleep(1.0)
            if self._stop:
                break
            tick += 1
            if self._reload:
                self._reload = False
                self.reload_si()
            self.engine_poll()
            for p in self.ingest:
                p.poll()
            for an in self.analyzers:
                an.poll()
            try:
                self.si.tick()
            except Exception as e:                          # noqa: BLE001
                log.exception("SI update failed: %s", e)
            if tick % 2 == 0:
                self.check_alerts()
        log.info("stopping")
        for p in self.ingest:
            p.stop()
        for an in self.analyzers:
            an.stop()
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

    def status(self):
        """everything the web page shows"""
        with self.lock:
            st = self.stats
        plan = self.plan
        out = {"version": __import__("dvbtx").__version__, "uptime": round(time.time() - self.started),
               "stats_age": round(time.time() - self.stats_at, 1) if self.stats_at else None,
               "engine": {"running": self.engine is not None and self.engine.poll() is None,
                          "restarts": self.engine_restarts, "log": list(self.engine_lines)[-15:]},
               "radios": [], "carriers": [], "inputs": [p.status() for p in self.ingest],
               "alerts": list(self.alerts.active.values()), "alert_history": list(self.alerts.history)[:50],
               "network": {k: plan.cfg["network"][k] for k in ("name", "network_id", "onid", "country", "provider")},
               "thresholds": {"margin_min_ms": self.cfg["alerts"]["margin_min_ms"]},
               "si": dict(self.si.stats), "epg_errors": self.store.errors[-5:] if self.store else []}
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
            for s in c.services:
                ls = live["services"][s.local_index] if live and s.local_index < len(live.get("services", [])) else None
                evs = self.store.events(s.sid) if self.store else []
                pres, fol, _ = epgmod.present_following(evs, dt.datetime.now(UTC))
                svcs.append({"name": s.name, "sid": s.sid, "lcn": s.lcn, "source": s.source, "backup": s.backup,
                             "type": self.si._stype(s), "live": ls,
                             "input": ist.get(s.input.index) if s.input else None,
                             "now": pres and {"title": pres.get("title"), "start": pres["start"].isoformat(),
                                              "duration": pres["duration"]},
                             "next": fol and {"title": fol.get("title"), "start": fol["start"].isoformat()},
                             "epg_events": len(evs)})
            an = next((a for a in self.analyzers if a.car is c), None)
            out["carriers"].append({"index": c.index, "name": c.name, "standard": c.standard, "frequency": c.frequency,
                                    "offset": c.offset, "params": c.params, "tsid": c.tsid, "capacity": c.ts_rate,
                                    "radio": c.radio.name if c.radio else None, "live": live, "services": svcs,
                                    "tsduck": an.summary() if an else None,
                                    "monitor": f"udp://127.0.0.1:{int(plan.cfg['ingest']['monitor_base_port']) + c.index}"})
        return out
